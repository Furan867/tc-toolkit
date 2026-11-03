// 共享聚合层：把采样到的 (IP, ThreadId) 变成 samples.tsv v1 的计数表。
// 两个后端（etw / suspend）只负责产出 (IP, ThreadId)，分桶与合并全在这里，所以
// 单测可以用合成序列喂满这一层，不需要游戏进程。
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hotspot {

// samples.tsv v1 的 bucket 语义（spec §4.4）。
enum class Bucket : std::uint8_t {
    blob = 0,       // 落在本产物全部副本内，blob_offset 有效
    other_exec = 1, // 其他非 IMAGE 可执行区（旧电路 JIT 残留等）
    image = 2,      // IMAGE 区（exe/dll，含游戏仿真主循环）
};

// 目标进程里一段可执行内存区（VirtualQueryEx 的结果）。
struct ExecRegion {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    bool image = false;
};

// 产物的一份内存副本；同一份产物可能有多份副本共存（spec §4.3 第 3 条）。
struct BlobCopy {
    std::uintptr_t base = 0;
    std::size_t code_size = 0;
};

struct SampleKey {
    Bucket bucket = Bucket::blob;
    std::uint64_t blob_offset = 0;
    std::uint32_t thread_id = 0;

    auto operator<=>(const SampleKey &) const = default;
};

// 单样本分桶。IP 不在任何可执行区（栈/堆/已释放）时返回 nullopt —— 这类样本丢弃，
// 不进任何桶。
// bucket 1/2 没有能写进 v1 四列的坐标（v1 只有 blob_offset），故 offset 记 0。
std::optional<SampleKey> classify(std::uintptr_t ip, std::uint32_t thread_id,
                                 std::span<const BlobCopy> copies,
                                 std::span<const ExecRegion> regions);

class SampleTable {
  public:
    void add(const SampleKey &key, std::uint64_t count = 1);

    // classify + add；样本在可执行区外则丢弃并返回 false。
    bool add_sample(std::uintptr_t ip, std::uint32_t thread_id, std::span<const BlobCopy> copies,
                    std::span<const ExecRegion> regions);

    std::uint64_t total() const;
    std::uint64_t count_in(Bucket bucket) const;
    void clear();

    const std::map<SampleKey, std::uint64_t> &rows() const
    {
        return rows_;
    }

    // samples.tsv v1：头两行 + 每个 (bucket, blob_offset, thread_id) 一行，按该键升序。
    std::string to_tsv() const;
    void write_tsv(std::ostream &out) const;

  private:
    std::map<SampleKey, std::uint64_t> rows_;
};

} // namespace hotspot
