/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "record_file.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <set>
#include <vector>

#include <base/logging.h>

#include "perf_event.h"
#include "record.h"
#include "utils.h"

using namespace PerfFileFormat;

std::unique_ptr<RecordFileReader> RecordFileReader::CreateInstance(const std::string& filename) {
  int fd = open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    PLOG(ERROR) << "failed to open record file '" << filename << "'";
    return nullptr;
  }
  auto reader = std::unique_ptr<RecordFileReader>(new RecordFileReader(filename, fd));
  if (!reader->MmapFile()) {
    return nullptr;
  }
  return reader;
}

RecordFileReader::RecordFileReader(const std::string& filename, int fd)
    : filename_(filename), record_fd_(fd), mmap_addr_(nullptr), mmap_len_(0) {
}

RecordFileReader::~RecordFileReader() {
  if (record_fd_ != -1) {
    Close();
  }
}

bool RecordFileReader::Close() {
  bool result = true;
  if (munmap(const_cast<char*>(mmap_addr_), mmap_len_) == -1) {
    PLOG(ERROR) << "failed to munmap() record file '" << filename_ << "'";
    result = false;
  }
  if (close(record_fd_) == -1) {
    PLOG(ERROR) << "failed to close record file '" << filename_ << "'";
    result = false;
  }
  record_fd_ = -1;
  return result;
}

bool RecordFileReader::MmapFile() {
  off_t file_size = lseek(record_fd_, 0, SEEK_END);
  if (file_size == -1) {
    return false;
  }
  size_t mmap_len = file_size;
  void* mmap_addr = mmap(nullptr, mmap_len, PROT_READ, MAP_SHARED, record_fd_, 0);
  if (mmap_addr == MAP_FAILED) {
    PLOG(ERROR) << "failed to mmap() record file '" << filename_ << "'";
    return false;
  }

  mmap_addr_ = reinterpret_cast<const char*>(mmap_addr);
  mmap_len_ = mmap_len;
  return true;
}

const FileHeader* RecordFileReader::FileHeader() {
  return reinterpret_cast<const struct FileHeader*>(mmap_addr_);
}

std::vector<const FileAttr*> RecordFileReader::AttrSection() {
  std::vector<const FileAttr*> result;
  const struct FileHeader* header = FileHeader();
  size_t attr_count = header->attrs.size / header->attr_size;
  const FileAttr* attr = reinterpret_cast<const FileAttr*>(mmap_addr_ + header->attrs.offset);
  for (size_t i = 0; i < attr_count; ++i) {
    result.push_back(attr++);
  }
  return result;
}

std::vector<uint64_t> RecordFileReader::IdsForAttr(const FileAttr* attr) {
  std::vector<uint64_t> result;
  size_t id_count = attr->ids.size / sizeof(uint64_t);
  const uint64_t* id = reinterpret_cast<const uint64_t*>(mmap_addr_ + attr->ids.offset);
  for (size_t i = 0; i < id_count; ++i) {
    result.push_back(*id++);
  }
  return result;
}

static bool IsRecordHappensBefore(const std::unique_ptr<const Record>& r1,
                                  const std::unique_ptr<const Record>& r2) {
  bool is_r1_sample = (r1->header.type == PERF_RECORD_SAMPLE);
  bool is_r2_sample = (r2->header.type == PERF_RECORD_SAMPLE);
  uint64_t time1 = (is_r1_sample ? static_cast<const SampleRecord*>(r1.get())->time_data.time
                                 : r1->sample_id.time_data.time);
  uint64_t time2 = (is_r2_sample ? static_cast<const SampleRecord*>(r2.get())->time_data.time
                                 : r2->sample_id.time_data.time);
  // The record with smaller time happens first.
  if (time1 != time2) {
    return time1 < time2;
  }
  // If happening at the same time, make non-sample records before sample records,
  // because non-sample records may contain useful information to parse sample records.
  if (is_r1_sample != is_r2_sample) {
    return is_r1_sample ? false : true;
  }
  // Otherwise, don't care of the order.
  return false;
}

std::vector<std::unique_ptr<const Record>> RecordFileReader::DataSection() {
  std::vector<std::unique_ptr<const Record>> result;
  const struct FileHeader* header = FileHeader();
  auto file_attrs = AttrSection();
  CHECK(file_attrs.size() > 0);
  perf_event_attr attr = file_attrs[0]->attr;

  const char* end = mmap_addr_ + header->data.offset + header->data.size;
  const char* p = mmap_addr_ + header->data.offset;
  while (p < end) {
    const perf_event_header* header = reinterpret_cast<const perf_event_header*>(p);
    if (p + header->size <= end) {
      result.push_back(std::move(ReadRecordFromBuffer(attr, header)));
    }
    p += header->size;
  }
  if ((attr.sample_type & PERF_SAMPLE_TIME) && attr.sample_id_all) {
    std::sort(result.begin(), result.end(), IsRecordHappensBefore);
  }
  return result;
}

const std::map<int, SectionDesc>& RecordFileReader::FeatureSectionDescriptors() {
  if (feature_sections_.empty()) {
    std::vector<int> features;
    const struct FileHeader* header = FileHeader();
    for (size_t i = 0; i < sizeof(header->features); ++i) {
      for (size_t j = 0; j < 8; ++j) {
        if (header->features[i] & (1 << j)) {
          features.push_back(i * 8 + j);
        }
      }
    }
    uint64_t feature_section_offset = header->data.offset + header->data.size;
    const SectionDesc* p = reinterpret_cast<const SectionDesc*>(mmap_addr_ + feature_section_offset);
    for (auto& feature : features) {
      feature_sections_.insert(std::make_pair(feature, *p));
      ++p;
    }
  }
  return feature_sections_;
}

std::vector<std::string> RecordFileReader::ReadCmdlineFeature() {
  const std::map<int, SectionDesc>& section_map = FeatureSectionDescriptors();
  auto it = section_map.find(FEAT_CMDLINE);
  if (it == section_map.end()) {
    return std::vector<std::string>();
  }
  SectionDesc section = it->second;
  const char* p = DataAtOffset(section.offset);
  const char* end = DataAtOffset(section.offset + section.size);
  std::vector<std::string> cmdline;
  uint32_t arg_count;
  MoveFromBinaryFormat(arg_count, p);
  CHECK_LE(p, end);
  for (size_t i = 0; i < arg_count; ++i) {
    uint32_t len;
    MoveFromBinaryFormat(len, p);
    CHECK_LE(p + len, end);
    cmdline.push_back(p);
    p += len;
  }
  return cmdline;
}