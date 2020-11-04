#pragma once

#include <city.h>
#include <type_traits>

#include <ext/bit_cast.h>

#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeTuple.h>

#include <Interpreters/AggregationCommon.h>

#include <Common/HashTable/Hash.h>
#include <Common/HashTable/HashSet.h>
#include <Common/HyperLogLogWithSmallSetOptimization.h>
#include <Common/CombinedCardinalityEstimator.h>
#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>

#include <AggregateFunctions/UniquesHashSet.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <AggregateFunctions/UniqVariadicHash.h>

#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif


namespace DB
{

/// uniq

struct AggregateFunctionUniqUniquesHashSetData
{
    using Set = UniquesHashSet<DefaultHash<UInt64>>;
    Set set;

    static String getName() { return "uniq"; }
};

/// For a function that takes multiple arguments. Such a function pre-hashes them in advance, so TrivialHash is used here.
struct AggregateFunctionUniqUniquesHashSetDataForVariadic
{
    using Set = UniquesHashSet<TrivialHash>;
    Set set;

    static String getName() { return "uniq"; }
};


/// uniqHLL12

template <typename T>
struct AggregateFunctionUniqHLL12Data
{
    using Set = HyperLogLogWithSmallSetOptimization<T, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

template <>
struct AggregateFunctionUniqHLL12Data<String>
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

template <>
struct AggregateFunctionUniqHLL12Data<UInt128>
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

struct AggregateFunctionUniqHLL12DataForVariadic
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12, TrivialHash>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};


/// uniqExact

template <typename T>
struct AggregateFunctionUniqExactData
{
    using Key = T;

    /// When creating, the hash table must be small.
    using Set = HashSet<
        Key,
        HashCRC32<Key>,
        HashTableGrower<4>,
        HashTableAllocatorWithStackMemory<sizeof(Key) * (1 << 4)>>;

    Set set;

    static String getName() { return "uniqExact"; }
};

/// For strings, we put the SipHash values (128 bits) into the hash table.
template <>
struct AggregateFunctionUniqExactData<String>
{
    using Key = UInt128;

    /// When creating, the hash table must be small.
    using Set = HashSet<
        Key,
        UInt128TrivialHash,
        HashTableGrower<3>,
        HashTableAllocatorWithStackMemory<sizeof(Key) * (1 << 3)>>;

    Set set;

    static String getName() { return "uniqExact"; }
};


namespace detail
{

/** Hash function for uniq.
  */
template <typename T> struct AggregateFunctionUniqTraits
{
    static UInt64 hash(T x)
    {
        if constexpr (std::is_same_v<T, UInt128>)
        {
            return sipHash64(x);
        }
        else if constexpr (std::is_same_v<T, Float32> || std::is_same_v<T, Float64>)
        {
            return ext::bit_cast<UInt64>(x);
        }
        else if constexpr (sizeof(T) <= sizeof(UInt64))
            return x;
        else
            return DefaultHash64<T>(x);
    }
};


/** The structure for the delegation work to add one element to the `uniq` aggregate functions.
  * Used for partial specialization to add strings.
  */
template <typename T, typename Data>
struct OneAdder
{
    static void ALWAYS_INLINE add(Data & data, const IColumn & column, size_t row_num)
    {
        if constexpr (std::is_same_v<Data, AggregateFunctionUniqUniquesHashSetData>
            || std::is_same_v<Data, AggregateFunctionUniqHLL12Data<T>>)
        {
            if constexpr (!std::is_same_v<T, String>)
            {
                const auto & value = assert_cast<const ColumnVector<T> &>(column).getElement(row_num);
                data.set.insert(AggregateFunctionUniqTraits<T>::hash(value));
            }
            else
            {
                StringRef value = column.getDataAt(row_num);
                data.set.insert(CityHash_v1_0_2::CityHash64(value.data, value.size));
            }
        }
        else if constexpr (std::is_same_v<Data, AggregateFunctionUniqExactData<T>>)
        {
            if constexpr (!std::is_same_v<T, String>)
            {
                data.set.insert(assert_cast<const ColumnVector<T> &>(column).getData()[row_num]);
            }
            else
            {
                StringRef value = column.getDataAt(row_num);

                UInt128 key;

#if defined(__SSSE3__) && !defined(MEMORY_SANITIZER)
                /// A trick for better performance: use last bit of key as a flag.
                /// If string is not larger than 15 bytes, set the flag to zero and put the string itself into the key.
                /// If it is larger - calculate it's cryptographic hash but set the last bit to one.

                if (value.size <= 15)
                {
                    /// We will do memcpy of value up to its size and memset to zero of the rest bytes.
                    /// It is possible to do it with a single "shuffle" instruction (and load, store).
                    /// Columns have 15 bytes padding, that's why it is safe to read 16 bytes.
                    /// But we have to disable it under memory sanitizer.

                    static constexpr Int8 __attribute__((__aligned__(16))) masks[] =
                    {
                       -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7, -1, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8, -1, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, -1, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, -1, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, -1, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, -1, -1,
                        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, -1,
                    };

                    _mm_storeu_si128(reinterpret_cast<__m128i *>(&key),
                        _mm_shuffle_epi8(
                            _mm_loadu_si128(reinterpret_cast<const __m128i *>(value.data)),
                            _mm_load_si128(reinterpret_cast<const __m128i *>(masks) + value.size)));

                    /// Apply some very light bijective hashing.
                    /// It is needed, because the hash table is using trivial hash (just key.low % table size).

                    key.low ^= key.low >> 33;
                    key.low *= 0xff51afd7ed558ccdULL;
                    /// Very similar to murmur finalizer, the only difference is that we mix higher part of strings also.
                    /// key.high remains untouched, that's why this step is also bijective.
                    key.low ^= key.high;
                    key.low ^= key.low >> 33;
                    key.low *= 0xc4ceb9fe1a85ec53ULL;
                    key.low ^= key.low >> 33;
                }
                else
#endif
                {
                    SipHash hash;
                    hash.update(value.data, value.size);
                    hash.get128(key.low, key.high);
#if defined(__SSSE3__) && !defined(MEMORY_SANITIZER)
                    key.high |= 0x8000000000000000ULL;  /// Assuming little endian.
#endif
                }

                data.set.insert(key);
            }
        }
    }
};

}


/// Calculates the number of different values approximately or exactly.
template <typename T, typename Data>
class AggregateFunctionUniq final : public IAggregateFunctionDataHelper<Data, AggregateFunctionUniq<T, Data>>
{
public:
    AggregateFunctionUniq(const DataTypes & argument_types_)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionUniq<T, Data>>(argument_types_, {}) {}

    String getName() const override { return Data::getName(); }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    /// ALWAYS_INLINE is required to have better code layout for uniqHLL12 function
    void ALWAYS_INLINE add(AggregateDataPtr place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        detail::OneAdder<T, Data>::add(this->data(place), *columns[0], row_num);
    }

    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).set.merge(this->data(rhs).set);
    }

    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
    {
        this->data(place).set.write(buf);
    }

    void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena *) const override
    {
        this->data(place).set.read(buf);
    }

    void insertResultInto(AggregateDataPtr place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).set.size());
    }
};


/** For multiple arguments. To compute, hashes them.
  * You can pass multiple arguments as is; You can also pass one argument - a tuple.
  * But (for the possibility of efficient implementation), you can not pass several arguments, among which there are tuples.
  */
template <typename Data, bool is_exact, bool argument_is_tuple>
class AggregateFunctionUniqVariadic final : public IAggregateFunctionDataHelper<Data, AggregateFunctionUniqVariadic<Data, is_exact, argument_is_tuple>>
{
private:
    size_t num_args = 0;

public:
    AggregateFunctionUniqVariadic(const DataTypes & arguments)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionUniqVariadic<Data, is_exact, argument_is_tuple>>(arguments, {})
    {
        if (argument_is_tuple)
            num_args = typeid_cast<const DataTypeTuple &>(*arguments[0]).getElements().size();
        else
            num_args = arguments.size();
    }

    String getName() const override { return Data::getName(); }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    void add(AggregateDataPtr place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        this->data(place).set.insert(typename Data::Set::value_type(
            UniqVariadicHash<is_exact, argument_is_tuple>::apply(num_args, columns, row_num)));
    }

    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).set.merge(this->data(rhs).set);
    }

    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
    {
        this->data(place).set.write(buf);
    }

    void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena *) const override
    {
        this->data(place).set.read(buf);
    }

    void insertResultInto(AggregateDataPtr place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).set.size());
    }
};

}
