//
// Created by vout on 11/24/18.
//

#ifndef MEGAHIT_ASYNC_SEQUENCE_READER_H
#define MEGAHIT_ASYNC_SEQUENCE_READER_H

#include <future>
#include <string>
#include "sequence/readers/binary_reader.h"
#include "sequence/readers/contig_reader.h"
#include "sequence/sequence_package.h"

template<class PackageType>
class AsyncSequenceReader {
 public:
  using package_type = PackageType;
  AsyncSequenceReader() = default;
  virtual ~AsyncSequenceReader() = default;
  package_type &Next() {
    auto &ret = next_pkg_.get();
    AsyncReadNextBatch();
    return ret;
  }

 protected:
  void AsyncReadNextBatch() {
    auto& pkg = packages_[batch_index_];
    next_pkg_ = std::async(
        std::launch::async,
        [this, &pkg]() -> package_type & {
          ReadOneBatch(&pkg);
          return pkg;
        });
    batch_index_ ^= 1;
  }
  void StopReading() {
    next_pkg_.wait();
  }
  virtual void ReadOneBatch(package_type *pkg) = 0;
 private:
  unsigned batch_index_{0};
  package_type packages_[2];
  std::future<package_type &> next_pkg_;
};

class AsyncContigReader : public AsyncSequenceReader<std::pair<SeqPackage, std::vector<float>>> {
 public:
  explicit AsyncContigReader(const std::string &file_name):
    reader_(file_name) {
    reader_.SetDiscardFlag(contig_flag::kLoop | contig_flag::kStandalone);
    AsyncReadNextBatch();
  }
  ~AsyncContigReader() override {
    StopReading();
  }

 protected:
  void ReadOneBatch(package_type *pkg) override {
    pkg->first.Clear();
    pkg->second.clear();
    const int64_t kMaxNumContigs = 1u << 22;
    const int64_t kMaxNumBases = 1u << 28;
    const bool reverse = false;
    reader_.ReadWithMultiplicity(&pkg->first, &pkg->second, kMaxNumContigs, kMaxNumBases, reverse);
  }

 private:
  ContigReader reader_;
};

class AsyncReadReader : public AsyncSequenceReader<SeqPackage> {
 public:
  explicit AsyncReadReader(const std::string &file_name) {
    reader_.reset(new BinaryReader(file_name));
    AsyncReadNextBatch();
  }
  ~AsyncReadReader() override {
    StopReading();
  }
 protected:
  void ReadOneBatch(SeqPackage *seq_pkg) override {
    int64_t kMaxNumReads = 1u << 22;
    int64_t kMaxNumBases = 1u << 28;
    bool reverse = false;
    seq_pkg->Clear();
    reader_->Read(seq_pkg, kMaxNumReads, kMaxNumBases, reverse);
  }
 private:
  std::unique_ptr<BinaryReader> reader_;
};

#endif //MEGAHIT_ASYNC_SEQUENCE_READER_H
