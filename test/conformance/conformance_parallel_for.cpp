/*
    Copyright (c) 2005-2023 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#define DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#include "common/test.h"
#include "common/utils.h"
#include "common/utils_report.h"
#include "common/type_requirements_test.h"

#include "oneapi/tbb/parallel_for.h"
#include "oneapi/tbb/tick_count.h"

#include "../tbb/test_partitioner.h"

#include <atomic>

//! \file conformance_parallel_for.cpp
//! \brief Test for [algorithms.parallel_for algorithms.auto_partitioner algorithms.simple_partitioner algorithms.static_partitioner algorithms.affinity_partitioner] specification

static const int N = 500;
static std::atomic<int> Array[N];

struct parallel_tag {};
struct empty_partitioner_tag {};

// Testing parallel_for with step support
const std::size_t PFOR_BUFFER_TEST_SIZE = 1024;
// test_buffer has some extra items beyond its right bound
const std::size_t PFOR_BUFFER_ACTUAL_SIZE = PFOR_BUFFER_TEST_SIZE + 1024;
size_t pfor_buffer[PFOR_BUFFER_ACTUAL_SIZE];

template<typename T>
class TestFunctor{
public:
    void operator ()(T index) const {
        pfor_buffer[index]++;
    }
};

static std::atomic<int> FooBodyCount;

// A range object whose only public members are those required by the Range concept.
template<size_t Pad>
class FooRange {
    // Start of range
    int start;

    // Size of range
    int size;
    FooRange( int start_, int size_ ) : start(start_), size(size_) {
        utils::zero_fill<char>(pad, Pad);
        pad[Pad-1] = 'x';
    }
    template<typename Flavor_, std::size_t Pad_> friend void Flog( );
    template<size_t Pad_> friend class FooBody;
    void operator&();

    char pad[Pad];
public:
    bool empty() const {return size==0;}
    bool is_divisible() const {return size>1;}
    FooRange( FooRange& original, oneapi::tbb::split ) : size(original.size/2) {
        original.size -= size;
        start = original.start+original.size;
        CHECK_FAST( original.pad[Pad-1]=='x');
        pad[Pad-1] = 'x';
    }
};

// A range object whose only public members are those required by the parallel_for.h body concept.
template<size_t Pad>
class FooBody {
public:
    ~FooBody() {
        --FooBodyCount;
        for( std::size_t i=0; i<sizeof(*this); ++i )
            reinterpret_cast<char*>(this)[i] = -1;
    }
    // Copy constructor
    FooBody( const FooBody& other ) : array(other.array), state(other.state) {
        ++FooBodyCount;
        CHECK_FAST(state == LIVE);
    }
    void operator()( FooRange<Pad>& r ) const {
        for (int k = r.start; k < r.start + r.size; ++k) {
            CHECK_FAST(array[k].load(std::memory_order_relaxed) == 0);
            array[k].store(1, std::memory_order_relaxed);
        }
    }
private:
    const int LIVE = 0x1234;
    std::atomic<int>* array;
    int state;
    friend class FooRange<Pad>;
    template<typename Flavor_, std::size_t Pad_> friend void Flog( );
    FooBody( std::atomic<int>* array_ ) : array(array_), state(LIVE) {}
};

template <typename Flavor, typename Partitioner, typename Range, typename Body>
struct Invoker;

template <typename Range, typename Body>
struct Invoker<parallel_tag, empty_partitioner_tag, Range, Body> {
    void operator()( const Range& r, const Body& body, empty_partitioner_tag& ) {
        oneapi::tbb::parallel_for( r, body );
    }
};

template <typename Partitioner, typename Range, typename Body>
struct Invoker<parallel_tag, Partitioner, Range, Body> {
    void operator()( const Range& r, const Body& body, Partitioner& p ) {
        oneapi::tbb::parallel_for( r, body, p );
    }
};

template <typename Flavor, typename Partitioner, typename T, typename Body>
struct InvokerStep;

template <typename T, typename Body>
struct InvokerStep<parallel_tag, empty_partitioner_tag, T, Body> {
    void operator()( const T& first, const T& last, const Body& f, empty_partitioner_tag& ) {
        oneapi::tbb::parallel_for( first, last, f );
    }
    void operator()( const T& first, const T& last, const T& step, const Body& f, empty_partitioner_tag& ) {
        oneapi::tbb::parallel_for( first, last, step, f );
    }
};

template <typename Partitioner, typename T, typename Body>
struct InvokerStep<parallel_tag, Partitioner, T, Body> {
    void operator()( const T& first, const T& last, const Body& f, Partitioner& p ) {
        oneapi::tbb::parallel_for( first, last, f, p );
    }
    void operator()( const T& first, const T& last, const T& step, const Body& f, Partitioner& p ) {
        oneapi::tbb::parallel_for( first, last, step, f, p );
    }
};

template<typename Flavor, std::size_t Pad>
void Flog() {
    for ( int i=0; i<N; ++i ) {
        for ( int mode = 0; mode < 4; ++mode) {
            FooRange<Pad> r( 0, i );
            const FooRange<Pad> rc = r;
            FooBody<Pad> f( Array );
            const FooBody<Pad> fc = f;
            for (int a_i = 0; a_i < N; a_i++) {
                Array[a_i].store(0, std::memory_order_relaxed);
            }
            FooBodyCount = 1;
            switch (mode) {
            case 0: {
                empty_partitioner_tag p;
                Invoker< Flavor, empty_partitioner_tag, FooRange<Pad>, FooBody<Pad> > invoke_for;
                invoke_for( rc, fc, p );
            }
                break;
            case 1: {
                Invoker< Flavor, const oneapi::tbb::simple_partitioner, FooRange<Pad>, FooBody<Pad> > invoke_for;
                invoke_for( rc, fc, oneapi::tbb::simple_partitioner() );
            }
                break;
            case 2: {
                Invoker< Flavor, const oneapi::tbb::auto_partitioner, FooRange<Pad>, FooBody<Pad> > invoke_for;
                invoke_for( rc, fc, oneapi::tbb::auto_partitioner() );
            }
                break;
            case 3: {
                static oneapi::tbb::affinity_partitioner affinity;
                Invoker< Flavor, oneapi::tbb::affinity_partitioner, FooRange<Pad>, FooBody<Pad> > invoke_for;
                invoke_for( rc, fc, affinity );
            }
                break;
            }
            CHECK(std::find_if_not(Array, Array + i, [](const std::atomic<int>& v) { return v.load(std::memory_order_relaxed) == 1; }) == Array + i);
            CHECK(std::find_if_not(Array + i, Array + N, [](const std::atomic<int>& v) { return v.load(std::memory_order_relaxed) == 0; }) == Array + N);
            CHECK(FooBodyCount == 1);
        }
    }
}

#include <stdexcept> // std::invalid_argument

template <typename Flavor, typename T, typename Partitioner>
void TestParallelForWithStepSupportHelper(Partitioner& p) {
    const T pfor_buffer_test_size = static_cast<T>(PFOR_BUFFER_TEST_SIZE);
    const T pfor_buffer_actual_size = static_cast<T>(PFOR_BUFFER_ACTUAL_SIZE);
    // Testing parallel_for with different step values
    InvokerStep< Flavor, Partitioner, T, TestFunctor<T> > invoke_for;
    for (T begin = 0; begin < pfor_buffer_test_size - 1; begin += pfor_buffer_test_size / 10 + 1) {
        T step;
        for (step = 1; step < pfor_buffer_test_size; step++) {
            std::memset(pfor_buffer, 0, pfor_buffer_actual_size * sizeof(std::size_t));
            if (step == 1){
                invoke_for(begin, pfor_buffer_test_size, TestFunctor<T>(), p);
            } else {
                invoke_for(begin, pfor_buffer_test_size, step, TestFunctor<T>(), p);
            }
            // Verifying that parallel_for processed all items it should
            for (T i = begin; i < pfor_buffer_test_size; i = i + step) {
                if (pfor_buffer[i] != 1) {
                    CHECK_MESSAGE(false, "parallel_for didn't process all required elements");
                }
                pfor_buffer[i] = 0;
            }
            // Verifying that no extra items were processed and right bound of array wasn't crossed
            for (T i = 0; i < pfor_buffer_actual_size; i++) {
                if (pfor_buffer[i] != 0) {
                    CHECK_MESSAGE(false, "parallel_for processed an extra element");
                }
            }
        }
    }
}

template <typename Flavor, typename T>
void TestParallelForWithStepSupport() {
    static oneapi::tbb::affinity_partitioner affinity_p;
    oneapi::tbb::auto_partitioner auto_p;
    oneapi::tbb::simple_partitioner simple_p;
    oneapi::tbb::static_partitioner static_p;
    empty_partitioner_tag p;

    // Try out all partitioner combinations
    TestParallelForWithStepSupportHelper< Flavor,T,empty_partitioner_tag >(p);
    TestParallelForWithStepSupportHelper< Flavor,T,const oneapi::tbb::auto_partitioner >(auto_p);
    TestParallelForWithStepSupportHelper< Flavor,T,const oneapi::tbb::simple_partitioner >(simple_p);
    TestParallelForWithStepSupportHelper< Flavor,T,oneapi::tbb::affinity_partitioner >(affinity_p);
    TestParallelForWithStepSupportHelper< Flavor,T,oneapi::tbb::static_partitioner >(static_p);

    // Testing some corner cases
    oneapi::tbb::parallel_for(static_cast<T>(2), static_cast<T>(1), static_cast<T>(1), TestFunctor<T>());
}

namespace test_req {

struct MinForBody : MinObj {
    using MinObj::MinObj;
    MinForBody(const MinForBody&) : MinObj(construct) {}
    ~MinForBody() {}

    void operator()(MinRange&) const {}
};

struct MinForIndex : MinObj {
    MinForIndex(int i) : MinObj(construct), real_index(i) {}
    MinForIndex(const MinForIndex& other) : MinObj(construct), real_index(other.real_index) {}
    ~MinForIndex() {}

    // Can return void by the spec, but implementation requires to return Index&
    MinForIndex& operator=(const MinForIndex& other) { real_index = other.real_index; return *this; }

    friend bool operator<(const MinForIndex& lhs, const MinForIndex& rhs) { return lhs.real_index < rhs.real_index; }
    friend std::size_t operator-(const MinForIndex& lhs, const MinForIndex& rhs) { return lhs.real_index - rhs.real_index; }

    friend MinForIndex operator+(const MinForIndex& idx, std::size_t k) { return MinForIndex{idx.real_index + int(k)}; }

    // Not included into the spec but required by the implementation
    friend bool operator<=(const MinForIndex& lhs, const MinForIndex& rhs) { return lhs.real_index <= rhs.real_index; }
    friend MinForIndex operator/(const MinForIndex& lhs, const MinForIndex& rhs) { return {lhs.real_index / rhs.real_index}; }
    friend MinForIndex operator+(const MinForIndex& lhs, const MinForIndex& rhs) { return {lhs.real_index + rhs.real_index}; }
    friend MinForIndex operator*(const MinForIndex& lhs, const MinForIndex& rhs) { return {lhs.real_index * rhs.real_index}; }

    MinForIndex& operator++() { ++real_index; return *this; }
    MinForIndex& operator+=(const MinForIndex& rhs) { real_index += rhs.real_index; return *this; }

private:
    int real_index;
};

struct MinForFunc : MinObj {
    using MinObj::MinObj;
    void operator()(MinForIndex) const {}
};

} // namespace test_req

template <typename... Args>
void run_parallel_for_overloads(const Args&... args) {
    oneapi::tbb::affinity_partitioner aff;
    oneapi::tbb::task_group_context ctx;

    oneapi::tbb::parallel_for(args...);
    oneapi::tbb::parallel_for(args..., oneapi::tbb::simple_partitioner{});
    oneapi::tbb::parallel_for(args..., oneapi::tbb::auto_partitioner{});
    oneapi::tbb::parallel_for(args..., oneapi::tbb::static_partitioner{});
    oneapi::tbb::parallel_for(args..., aff);

    oneapi::tbb::parallel_for(args..., ctx);
    oneapi::tbb::parallel_for(args..., oneapi::tbb::simple_partitioner{}, ctx);
    oneapi::tbb::parallel_for(args..., oneapi::tbb::auto_partitioner{}, ctx);
    oneapi::tbb::parallel_for(args..., oneapi::tbb::static_partitioner{}, ctx);
    oneapi::tbb::parallel_for(args..., aff, ctx);
}

//! Test simple parallel_for with different partitioners
//! \brief \ref interface \ref requirement
TEST_CASE("Basic parallel_for") {
    std::atomic<unsigned long> counter{};
    const std::size_t number_of_partitioners = 5;
    const std::size_t iterations = 100000;

    oneapi::tbb::parallel_for(std::size_t(0), iterations, [&](std::size_t) {
        counter++;
    });

    oneapi::tbb::parallel_for(std::size_t(0), iterations, [&](std::size_t) {
        counter++;
    }, oneapi::tbb::simple_partitioner());

    oneapi::tbb::parallel_for(std::size_t(0), iterations, [&](std::size_t) {
        counter++;
    }, oneapi::tbb::auto_partitioner());

    oneapi::tbb::parallel_for(std::size_t(0), iterations, [&](std::size_t) {
        counter++;
    }, oneapi::tbb::static_partitioner());

    oneapi::tbb::affinity_partitioner aff;
    oneapi::tbb::parallel_for(std::size_t(0), iterations, [&](std::size_t) {
        counter++;
    }, aff);

    CHECK_EQ(counter.load(std::memory_order_relaxed), iterations * number_of_partitioners);
}

//! Testing parallel for with different partitioners and ranges ranges
//! \brief \ref interface \ref requirement \ref stress
TEST_CASE("Flog test") {
    Flog<parallel_tag, 1>();
    Flog<parallel_tag, 10>();
    Flog<parallel_tag, 100>();
    Flog<parallel_tag, 1000>();
    Flog<parallel_tag, 10000>();
}

//! Testing parallel for with different types and step
//! \brief \ref interface \ref requirement
TEST_CASE_TEMPLATE("parallel_for with step support", T, short, unsigned short, int, unsigned int,
                                    long, unsigned long, long long, unsigned long long, std::size_t) {
    // Testing with different integer types
    TestParallelForWithStepSupport<parallel_tag, T>();
}

//! Testing with different types of ranges and partitioners
//! \brief \ref interface \ref requirement
TEST_CASE("Testing parallel_for with partitioners") {
    using namespace test_partitioner_utils::interaction_with_range_and_partitioner;

    test_partitioner_utils::SimpleBody b;
    oneapi::tbb::affinity_partitioner ap;

    parallel_for(Range1(true, false), b, ap);
    parallel_for(Range6(false, true), b, ap);

    parallel_for(Range1(false, true), b, oneapi::tbb::simple_partitioner());
    parallel_for(Range6(false, true), b, oneapi::tbb::simple_partitioner());

    parallel_for(Range1(false, true), b, oneapi::tbb::auto_partitioner());
    parallel_for(Range6(false, true), b, oneapi::tbb::auto_partitioner());

    parallel_for(Range1(true, false), b, oneapi::tbb::static_partitioner());
    parallel_for(Range6(false, true), b, oneapi::tbb::static_partitioner());
}

//! Testing parallel_for type requirements
//! \brief \ref requirement
TEST_CASE("parallel_for type requirements") {
    test_req::MinRange   range(test_req::construct);
    test_req::MinForBody body(test_req::construct);
    test_req::MinForFunc func(test_req::construct);

    test_req::MinForIndex index{1}, stride{1};

    run_parallel_for_overloads(range, body);
    run_parallel_for_overloads(index, index, func);
    run_parallel_for_overloads(index, index, stride, func);
}
