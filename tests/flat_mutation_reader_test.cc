/*
 * Copyright (C) 2017 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <seastar/core/thread.hh>
#include <seastar/tests/test-utils.hh>

#include "mutation.hh"
#include "streamed_mutation.hh"
#include "mutation_source_test.hh"
#include "flat_mutation_reader.hh"
#include "mutation_reader.hh"
#include "schema_builder.hh"
#include "memtable.hh"
#include "mutation_reader_assertions.hh"
#include "row_cache.hh"
#include "sstables/sstables.hh"
#include "tmpdir.hh"
#include "sstable_test.hh"

#include "disk-error-handler.hh"

thread_local disk_error_signal_type commit_error;
thread_local disk_error_signal_type general_disk_error;

static void test_double_conversion_through_mutation_reader(const std::vector<mutation>& mutations) {
    BOOST_REQUIRE(!mutations.empty());
    auto schema = mutations[0].schema();
    auto base_reader = make_reader_returning_many(mutations);
    auto flat_reader = flat_mutation_reader_from_mutation_reader(schema,
                                                                 std::move(base_reader),
                                                                 streamed_mutation::forwarding::no);
    auto normal_reader = mutation_reader_from_flat_mutation_reader(schema, std::move(flat_reader));
    for (auto& m : mutations) {
        auto smopt = normal_reader().get0();
        BOOST_REQUIRE(smopt);
        auto mopt = mutation_from_streamed_mutation(std::move(*smopt)).get0();
        BOOST_REQUIRE(mopt);
        BOOST_REQUIRE_EQUAL(m, *mopt);
    }
    BOOST_REQUIRE(!normal_reader().get0());
}

static void check_two_readers_are_the_same(schema_ptr schema, mutation_reader& normal_reader, flat_mutation_reader& flat_reader) {
    auto smopt = normal_reader().get0();
    BOOST_REQUIRE(smopt);
    auto mfopt = flat_reader().get0();
    BOOST_REQUIRE(mfopt);
    BOOST_REQUIRE(mfopt->is_partition_start());
    BOOST_REQUIRE(smopt->decorated_key().equal(*schema, mfopt->as_mutable_partition_start().key()));
    BOOST_REQUIRE_EQUAL(smopt->partition_tombstone(), mfopt->as_mutable_partition_start().partition_tombstone());
    mutation_fragment_opt sm_mfopt;
    while (bool(sm_mfopt = (*smopt)().get0())) {
        mfopt = flat_reader().get0();
        BOOST_REQUIRE(mfopt);
        BOOST_REQUIRE(sm_mfopt->equal(*schema, *mfopt));
    }
    mfopt = flat_reader().get0();
    BOOST_REQUIRE(mfopt);
    BOOST_REQUIRE(mfopt->is_end_of_partition());
}

static void test_conversion_to_flat_mutation_reader_through_mutation_reader(const std::vector<mutation>& mutations) {
    BOOST_REQUIRE(!mutations.empty());
    auto schema = mutations[0].schema();
    auto base_reader = make_reader_returning_many(mutations);
    auto flat_reader = flat_mutation_reader_from_mutation_reader(schema,
                                                                 std::move(base_reader),
                                                                 streamed_mutation::forwarding::no);
    for (auto& m : mutations) {
        auto normal_reader = make_reader_returning(m);
        check_two_readers_are_the_same(schema, normal_reader, flat_reader);
    }
}

static void test_conversion(const std::vector<mutation>& mutations) {
    BOOST_REQUIRE(!mutations.empty());
    auto schema = mutations[0].schema();
    auto flat_reader = flat_mutation_reader_from_mutations(std::vector<mutation>(mutations), streamed_mutation::forwarding::no);
    for (auto& m : mutations) {
        mutation_opt m2 = read_mutation_from_flat_mutation_reader(schema, flat_reader).get0();
        BOOST_REQUIRE(m2);
        BOOST_REQUIRE_EQUAL(m, *m2);
    }
    BOOST_REQUIRE(!read_mutation_from_flat_mutation_reader(schema, flat_reader).get0());
}

/*
 * =================
 * ===== Tests =====
 * =================
 */

SEASTAR_TEST_CASE(test_conversions_through_mutation_reader_single_mutation) {
    return seastar::async([] {
        for_each_mutation([&] (const mutation& m) {
            test_double_conversion_through_mutation_reader({m});
            test_conversion_to_flat_mutation_reader_through_mutation_reader({m});
        });
    });
}

SEASTAR_TEST_CASE(test_double_conversion_through_mutation_reader_two_mutations) {
    return seastar::async([] {
        for_each_mutation_pair([&] (auto&& m, auto&& m2, are_equal) {
            if (m.decorated_key().less_compare(*m.schema(), m2.decorated_key())) {
                test_double_conversion_through_mutation_reader({m, m2});
                test_conversion_to_flat_mutation_reader_through_mutation_reader({m, m2});
            } else if (m2.decorated_key().less_compare(*m.schema(), m.decorated_key())) {
                test_double_conversion_through_mutation_reader({m2, m});
                test_conversion_to_flat_mutation_reader_through_mutation_reader({m2, m});
            }
        });
    });
}

SEASTAR_TEST_CASE(test_conversions_single_mutation) {
    return seastar::async([] {
        for_each_mutation([&] (const mutation& m) {
            test_conversion({m});
        });
    });
}

SEASTAR_TEST_CASE(test_double_conversion_two_mutations) {
    return seastar::async([] {
        for_each_mutation_pair([&] (auto&& m, auto&& m2, are_equal) {
            if (m.decorated_key().less_compare(*m.schema(), m2.decorated_key())) {
                test_conversion({m, m2});
            } else if (m2.decorated_key().less_compare(*m.schema(), m.decorated_key())) {
                test_conversion({m2, m});
            }
        });
    });
}

struct mock_consumer {
    struct result {
        size_t _depth;
        size_t _consume_new_partition_call_count = 0;
        size_t _consume_tombstone_call_count = 0;
        size_t _consume_end_of_partition_call_count = 0;
        bool _consume_end_of_stream_called = false;
        std::vector<mutation_fragment> _fragments;
    };
    result _result;
    mock_consumer(size_t depth) {
        _result._depth = depth;
    }
    stop_iteration update_depth() {
        --_result._depth;
        return _result._depth < 1 ? stop_iteration::yes : stop_iteration::no;
    }
    void consume_new_partition(const dht::decorated_key& dk) {
        ++_result._consume_new_partition_call_count;
    }
    stop_iteration consume(tombstone t) {
        ++_result._consume_tombstone_call_count;
        return stop_iteration::no;
    }
    stop_iteration consume(static_row&& sr) {
        _result._fragments.push_back(mutation_fragment(std::move(sr)));
        return update_depth();
    }
    stop_iteration consume(clustering_row&& cr) {
        _result._fragments.push_back(mutation_fragment(std::move(cr)));
        return update_depth();
    }
    stop_iteration consume(range_tombstone&& rt) {
        _result._fragments.push_back(mutation_fragment(std::move(rt)));
        return update_depth();
    }
    stop_iteration consume_end_of_partition() {
        ++_result._consume_end_of_partition_call_count;
        return update_depth();
    }
    result consume_end_of_stream() {
        _result._consume_end_of_stream_called = true;
        return _result;
    }
};

static size_t count_fragments(mutation m) {
    auto r = flat_mutation_reader_from_mutations({m}, streamed_mutation::forwarding::no);
    size_t res = 0;
    auto mfopt = r().get0();
    while (bool(mfopt)) {
        ++res;
        mfopt = r().get0();
    }
    return res;
}

SEASTAR_TEST_CASE(test_flat_mutation_reader_consume_single_partition) {
    return seastar::async([] {
        for_each_mutation([&] (const mutation& m) {
            size_t fragments_in_m = count_fragments(m);
            for (size_t depth = 1; depth <= fragments_in_m + 1; ++depth) {
                auto r = flat_mutation_reader_from_mutations({m}, streamed_mutation::forwarding::no);
                auto result = r.consume(mock_consumer(depth)).get0();
                BOOST_REQUIRE(result._consume_end_of_stream_called);
                BOOST_REQUIRE_EQUAL(1, result._consume_new_partition_call_count);
                BOOST_REQUIRE_EQUAL(1, result._consume_end_of_partition_call_count);
                BOOST_REQUIRE_EQUAL(m.partition().partition_tombstone() ? 1 : 0, result._consume_tombstone_call_count);
                auto r2 = flat_mutation_reader_from_mutations({m}, streamed_mutation::forwarding::no);
                auto start = r2().get0();
                BOOST_REQUIRE(start);
                BOOST_REQUIRE(start->is_partition_start());
                for (auto& mf : result._fragments) {
                    auto mfopt = r2().get0();
                    BOOST_REQUIRE(mfopt);
                    BOOST_REQUIRE(mf.equal(*m.schema(), *mfopt));
                }
            }
        });
    });
}

SEASTAR_TEST_CASE(test_flat_mutation_reader_consume_two_partitions) {
    return seastar::async([] {
        auto test = [] (mutation m1, mutation m2) {
            size_t fragments_in_m1 = count_fragments(m1);
            size_t fragments_in_m2 = count_fragments(m2);
            for (size_t depth = 1; depth < fragments_in_m1; ++depth) {
                auto r = flat_mutation_reader_from_mutations({m1, m2}, streamed_mutation::forwarding::no);
                auto result = r.consume(mock_consumer(depth)).get0();
                BOOST_REQUIRE(result._consume_end_of_stream_called);
                BOOST_REQUIRE_EQUAL(1, result._consume_new_partition_call_count);
                BOOST_REQUIRE_EQUAL(1, result._consume_end_of_partition_call_count);
                BOOST_REQUIRE_EQUAL(m1.partition().partition_tombstone() ? 1 : 0, result._consume_tombstone_call_count);
                auto r2 = flat_mutation_reader_from_mutations({m1, m2}, streamed_mutation::forwarding::no);
                auto start = r2().get0();
                BOOST_REQUIRE(start);
                BOOST_REQUIRE(start->is_partition_start());
                for (auto& mf : result._fragments) {
                    auto mfopt = r2().get0();
                    BOOST_REQUIRE(mfopt);
                    BOOST_REQUIRE(mf.equal(*m1.schema(), *mfopt));
                }
            }
            for (size_t depth = fragments_in_m1; depth < fragments_in_m1 + fragments_in_m2 + 1; ++depth) {
                auto r = flat_mutation_reader_from_mutations({m1, m2}, streamed_mutation::forwarding::no);
                auto result = r.consume(mock_consumer(depth)).get0();
                BOOST_REQUIRE(result._consume_end_of_stream_called);
                BOOST_REQUIRE_EQUAL(2, result._consume_new_partition_call_count);
                BOOST_REQUIRE_EQUAL(2, result._consume_end_of_partition_call_count);
                size_t tombstones_count = 0;
                if (m1.partition().partition_tombstone()) {
                    ++tombstones_count;
                }
                if (m2.partition().partition_tombstone()) {
                    ++tombstones_count;
                }
                BOOST_REQUIRE_EQUAL(tombstones_count, result._consume_tombstone_call_count);
                auto r2 = flat_mutation_reader_from_mutations({m1, m2}, streamed_mutation::forwarding::no);
                auto start = r2().get0();
                BOOST_REQUIRE(start);
                BOOST_REQUIRE(start->is_partition_start());
                for (auto& mf : result._fragments) {
                    auto mfopt = r2().get0();
                    BOOST_REQUIRE(mfopt);
                    if (mfopt->is_partition_start() || mfopt->is_end_of_partition()) {
                        mfopt = r2().get0();
                    }
                    BOOST_REQUIRE(mfopt);
                    BOOST_REQUIRE(mf.equal(*m1.schema(), *mfopt));
                }
            }
        };
        for_each_mutation_pair([&] (auto&& m, auto&& m2, are_equal) {
            if (m.decorated_key().less_compare(*m.schema(), m2.decorated_key())) {
                test(m, m2);
            } else if (m2.decorated_key().less_compare(*m.schema(), m.decorated_key())) {
                test(m2, m);
            }
        });
    });
}