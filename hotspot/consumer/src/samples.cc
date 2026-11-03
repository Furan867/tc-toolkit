#include "samples.hh"

#include <ostream>
#include <sstream>

namespace hotspot {
namespace {

bool contains(const ExecRegion &region, std::uintptr_t ip)
{
    return ip >= region.base && ip - region.base < region.size;
}

} // namespace

std::optional<SampleKey> classify(std::uintptr_t ip, std::uint32_t thread_id,
                                 std::span<const BlobCopy> copies,
                                 std::span<const ExecRegion> regions)
{
    // 副本先看：同一份产物多副本共存时，RIP − 代码起点的偏移对所有副本相同，
    // 所以取哪一个命中都得到同一个坐标，计数合并是自然的（spec §4.3）。
    std::size_t blob_hits = 0;
    std::optional<SampleKey> blob_hit;
    for (const BlobCopy &copy : copies) {
        if (ip >= copy.base && ip - copy.base < copy.code_size) {
            ++blob_hits;
            blob_hit = SampleKey{Bucket::blob, static_cast<std::uint64_t>(ip - copy.base),
                                 thread_id};
        }
    }
    if (blob_hit.has_value()) {
        // 副本区间不重叠是前提；重叠意味着定位或产物有错，别把样本算到别的副本上。
        return blob_hits == 1 ? blob_hit : std::nullopt;
    }

    for (const ExecRegion &region : regions) {
        if (contains(region, ip)) {
            return SampleKey{region.image ? Bucket::image : Bucket::other_exec, 0, thread_id};
        }
    }
    return std::nullopt;
}

void SampleTable::add(const SampleKey &key, std::uint64_t count)
{
    if (count == 0) {
        return;
    }
    rows_[key] += count;
}

bool SampleTable::add_sample(std::uintptr_t ip, std::uint32_t thread_id,
                            std::span<const BlobCopy> copies,
                            std::span<const ExecRegion> regions)
{
    const std::optional<SampleKey> key = classify(ip, thread_id, copies, regions);
    if (!key.has_value()) {
        return false;
    }
    add(*key);
    return true;
}

std::uint64_t SampleTable::total() const
{
    std::uint64_t sum = 0;
    for (const auto &[key, count] : rows_) {
        (void)key;
        sum += count;
    }
    return sum;
}

std::uint64_t SampleTable::count_in(Bucket bucket) const
{
    std::uint64_t sum = 0;
    for (const auto &[key, count] : rows_) {
        if (key.bucket == bucket) {
            sum += count;
        }
    }
    return sum;
}

void SampleTable::clear()
{
    rows_.clear();
}

std::string SampleTable::to_tsv() const
{
    std::ostringstream out;
    write_tsv(out);
    return out.str();
}

void SampleTable::write_tsv(std::ostream &out) const
{
    out << "# samples.tsv v1\n";
    out << "bucket\tblob_offset\tthread_id\tcount\n";
    for (const auto &[key, count] : rows_) {
        out << static_cast<unsigned>(key.bucket) << '\t' << key.blob_offset << '\t'
            << key.thread_id << '\t' << count << '\n';
    }
}

} // namespace hotspot
