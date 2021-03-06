//
// Created by vout on 11/23/18.
//

#ifndef MEGAHIT_JUNCION_INDEX_H
#define MEGAHIT_JUNCION_INDEX_H

#include <mutex>
#include <sdbg/sdbg_def.h>
#include <utils/utils.h>
#include "sequence/kmer_plus.h"
#include "sparsepp/spp.h"
#include "sequence/sequence_package.h"

template<class KmerType>
class ContigFlankIndex {
 public:
  struct FlankInfo {
    uint64_t ext_seq : 58;
    unsigned ext_len : 6;
    float mul;
  } __attribute__((packed));
  using Flank = KmerPlus<KmerType, FlankInfo>;
  using HashSet = spp::sparse_hash_set<Flank, KmerHash>;
 public:
  ContigFlankIndex(unsigned k, unsigned step) : k_(k), step_(step) {}
  size_t size() const { return hash_index_.size(); }

  void FeedBatchContigs(SeqPackage &seq_pkg, const std::vector<float> &mul) {
    std::mutex lock;
#pragma omp parallel for
    for (size_t i = 0; i < seq_pkg.Size(); ++i) {
      size_t seq_len = seq_pkg.SequenceLength(i);
      if (seq_len < k_ + 1) {
        continue;
      }
      for (int strand = 0; strand < 2; ++strand) {
        auto get_jth_char = [&seq_pkg, i, strand, seq_len](unsigned j) -> uint8_t {
          uint8_t c = seq_pkg.GetBase(i, strand == 0 ? j : (seq_len - 1 - j));
          return strand == 0 ? c : 3 ^ c;
        };

        KmerType kmer;
        for (unsigned j = 0; j < k_ + 1; ++j) {
          kmer.ShiftAppend(get_jth_char(j), k_ + 1);
        }
        if (kmer.IsPalindrome(k_ + 1)) {
          continue;
        }

        unsigned ext_len = std::min(static_cast<size_t>(step_ - 1), seq_len - (k_ + 1));
        uint64_t ext_seq = 0;
        for (unsigned j = 0; j < ext_len; ++j) {
          ext_seq |= uint64_t(get_jth_char(k_ + 1 + j)) << j * 2;
        }

        {
          std::lock_guard<std::mutex> lk(lock);
          auto res = hash_index_.emplace(kmer, FlankInfo{ext_seq, ext_len});
          if (!res.second) {
            auto old_len = res.first->aux.ext_len;
            auto old_seq = res.first->aux.ext_seq;
            if (old_len < ext_len || (old_len == ext_len && old_seq < ext_seq)) {
              hash_index_.erase(res.first);
              res = hash_index_.emplace(kmer, FlankInfo{ext_seq, ext_len});
              assert(res.second);
            }
          }
        }
        if (seq_len == k_ + 1) {
          break;
        }
      }
    }
  }

  template<class CollectorType>
  size_t FindNextKmersFromRead(SeqPackage &seq_pkg, size_t seq_id, CollectorType *out) const {
    size_t length = seq_pkg.SequenceLength(seq_id);
    if (length < k_ + step_ + 1) {
      return 0;
    }

    size_t num_success = 0;
    std::vector<bool> kmer_exist(length, false);
    std::vector<float> kmer_mul(length, 0);

    Flank flank, rflank;
    auto &kmer = flank.kmer;
    auto &rkmer = rflank.kmer;

    for (unsigned j = 0; j < k_ + 1; ++j) {
      kmer.ShiftAppend(seq_pkg.GetBase(seq_id, j), k_ + 1);
    }
    rkmer = kmer;
    rkmer.ReverseComplement(k_ + 1);

    unsigned cur_pos = 0;
    while (cur_pos + k_ + 1 <= length) {
      unsigned next_pos = cur_pos + 1;

      if (!kmer_exist[cur_pos]) {
        auto iter = hash_index_.find(flank);
        if (iter != hash_index_.end()) {
          kmer_exist[cur_pos] = true;
          uint64_t ext_seq = iter->aux.ext_seq;
          unsigned ext_len = iter->aux.ext_len;
          float mul = iter->aux.mul;
          kmer_mul[cur_pos] = mul;

          for (unsigned j = 0; j < ext_len && cur_pos + k_ + 1 + j < length; ++j, ++next_pos) {
            if (seq_pkg.GetBase(seq_id, cur_pos + k_ + 1 + j) == ((ext_seq >> j * 2) & 3)) {
              kmer_exist[cur_pos + j + 1] = true;
              kmer_mul[cur_pos + j + 1] = mul;
            } else {
              break;
            }
          }
        }
        if ((iter = hash_index_.find(rflank)) != hash_index_.end()) {
          uint64_t ext_seq = iter->aux.ext_seq;
          unsigned ext_len = iter->aux.ext_len;
          float mul = iter->aux.mul;
          kmer_mul[cur_pos] = kmer_exist[cur_pos] ? (kmer_mul[cur_pos] + mul) / 2 : mul;
          kmer_exist[cur_pos] = true;

          for (unsigned j = 0; j < ext_len && cur_pos >= j + 1; ++j) {
            if ((3 ^ seq_pkg.GetBase(seq_id, cur_pos - 1 - j)) == ((ext_seq >> j * 2) & 3)) {
              kmer_mul[cur_pos - 1 - j] = kmer_exist[cur_pos - 1 - j] ? (kmer_mul[cur_pos - 1 - j] + mul) / 2 : mul;
              kmer_exist[cur_pos - 1 - j] = true;
            } else {
              break;
            }
          }
        }
      }

      if (next_pos + k_ + 1 <= length) {
        while (cur_pos < next_pos) {
          ++cur_pos;
          uint8_t c = seq_pkg.GetBase(seq_id, cur_pos + k_);
          kmer.ShiftAppend(c, k_ + 1);
          rkmer.ShiftPreappend(3 ^ c, k_ + 1);
        }
      } else {
        break;
      }
    }

    for (unsigned j = 1; j + k_ + 1 <= length; ++j) {
      kmer_mul[j] += kmer_mul[j - 1];
    }

    typename CollectorType::kmer_type new_kmer, new_rkmer;

    for (unsigned accumulated_len = 0, j = 0, end_pos = 0; j + k_ < length; ++j) {
      accumulated_len = kmer_exist[j] ? accumulated_len + 1 : 0;
      if (accumulated_len >= step_ + 1) {
        unsigned target_end = j + k_ + 1;
        if (end_pos + 8 < target_end) {
          while (end_pos < target_end) {
            auto c = seq_pkg.GetBase(seq_id, end_pos);
            new_kmer.ShiftAppend(c, k_ + step_ + 1);
            new_rkmer.ShiftPreappend(3 ^ c, k_ + step_ + 1);
            end_pos++;
          }
        } else {
          if (end_pos + k_ + step_ + 1 < target_end) {
            end_pos = target_end - (k_ + step_ + 1);
          }
          while (end_pos < target_end) {
            auto c = seq_pkg.GetBase(seq_id, end_pos);
            new_kmer.ShiftAppend(c, k_ + step_ + 1);
            end_pos++;
          }
          new_rkmer = new_kmer;
          new_rkmer.ReverseComplement(k_ + step_ + 1);
        }
        float mul = (kmer_mul[j] - (j >= step_ + 1 ? kmer_mul[j - (step_ + 1)] : 0)) / (step_ + 1);
        assert(mul <= kMaxMul + 1);
        out->Insert(new_kmer < new_rkmer ? new_kmer : new_rkmer,
                    static_cast<mul_t>(std::min(kMaxMul, static_cast<int>(mul + 0.5))));
        num_success++;
      }
    }
    return num_success;
  }
 private:
  HashSet hash_index_;
  unsigned k_{};
  unsigned step_{};
};

#endif //MEGAHIT_JUNCION_INDEX_H
