// SPDX-License-Identifier: GPL-3.0-or-later

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../../src/gui/views/app.hpp"

// AppState queue operations only touch queue, queue_index, and queue_mtx.
// Engine/library/dbus pointers stay null throughout; none of the tested
// methods dereference them.

namespace {

void fill(transporter::gui::AppState& st,
          std::initializer_list<const char*> paths, std::int32_t current) {
    for (const auto* p : paths) {
        st.queue.push_back(p);
    }
    st.queue_index = current;
}

} // namespace

TEST_CASE("queue_move: adjacent swap preserves current track") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1); // b is current
    st.queue_move(0, 1);
    REQUIRE(st.queue.size() == 3);
    CHECK(st.queue[0].string() == "/b");
    CHECK(st.queue[1].string() == "/a");
    CHECK(st.queue[2].string() == "/c");
    CHECK(st.queue_index == 0); // b moved to slot 0
}

TEST_CASE("queue_move_to_top: moves element to front, adjusts current") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c", "/d"}, 2); // c is current
    st.queue_move_to_top(2);
    REQUIRE(st.queue.size() == 4);
    CHECK(st.queue[0].string() == "/c");
    CHECK(st.queue[1].string() == "/a");
    CHECK(st.queue[2].string() == "/b");
    CHECK(st.queue[3].string() == "/d");
    CHECK(st.queue_index == 0); // c is still current at its new position
}

TEST_CASE("queue_move_to_top: non-current element shifts current right") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 0); // a is current
    st.queue_move_to_top(2);          // move c to top
    CHECK(st.queue[0].string() == "/c");
    CHECK(st.queue[1].string() == "/a");
    CHECK(st.queue[2].string() == "/b");
    CHECK(st.queue_index == 1); // a shifted right by one
}

TEST_CASE("queue_move_to_top: no-op when already at top") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1);
    st.queue_move_to_top(0);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue_index == 1); // unchanged
}

TEST_CASE("queue_move_to_bottom: moves element to end, adjusts current") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c", "/d"}, 1); // b is current
    st.queue_move_to_bottom(1);
    REQUIRE(st.queue.size() == 4);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue[1].string() == "/c");
    CHECK(st.queue[2].string() == "/d");
    CHECK(st.queue[3].string() == "/b");
    CHECK(st.queue_index == 3); // b is still current at its new position
}

TEST_CASE("queue_move_to_bottom: non-current element shifts current left") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 2); // c is current
    st.queue_move_to_bottom(0);       // move a to bottom
    CHECK(st.queue[0].string() == "/b");
    CHECK(st.queue[1].string() == "/c");
    CHECK(st.queue[2].string() == "/a");
    CHECK(st.queue_index == 1); // c shifted left by one
}

TEST_CASE("queue_move_to_bottom: no-op when already at bottom") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1);
    st.queue_move_to_bottom(2);
    CHECK(st.queue[2].string() == "/c");
    CHECK(st.queue_index == 1); // unchanged
}

TEST_CASE("queue_remove: adjusts index for slot removed before current") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 2); // c is current
    st.queue_remove(0);               // remove a
    REQUIRE(st.queue.size() == 2);
    CHECK(st.queue[0].string() == "/b");
    CHECK(st.queue[1].string() == "/c");
    CHECK(st.queue_index == 1); // c shifted left
}

TEST_CASE("queue_remove: removing current clamps index to previous") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1); // b is current
    st.queue_remove(1);               // remove b (current)
    REQUIRE(st.queue.size() == 2);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue[1].string() == "/c");
    CHECK(st.queue_index == 0); // clamped to first remaining
}

TEST_CASE("queue_remove: removing current at index 0 advances to next") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 0); // a is current
    st.queue_remove(0);               // remove a
    REQUIRE(st.queue.size() == 2);
    CHECK(st.queue[0].string() == "/b");
    CHECK(st.queue_index == 0); // clamped to 0; b is now current
}

TEST_CASE("queue_remove: last track results in empty queue") {
    transporter::gui::AppState st;
    fill(st, {"/a"}, 0);
    st.queue_remove(0);
    CHECK(st.queue.empty());
    CHECK(st.queue_index == -1);
}

TEST_CASE("queue_peek_next: repeat-one returns current track") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1); // b is current
    st.repeat_mode = transporter::gui::AppState::RepeatMode::One;
    const auto p = st.queue_peek_next();
    REQUIRE(p.has_value());
    CHECK(p->string() == "/b");
}

TEST_CASE("queue_peek_next: repeat-all wraps at end") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 2); // c is current (last)
    st.repeat_mode = transporter::gui::AppState::RepeatMode::All;
    const auto p = st.queue_peek_next();
    REQUIRE(p.has_value());
    CHECK(p->string() == "/a"); // wraps to first
}

TEST_CASE("queue_previous: repeat-all wraps from first to last") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 0); // a is current (first)
    st.repeat_mode = transporter::gui::AppState::RepeatMode::All;
    const auto p = st.queue_previous();
    REQUIRE(p.has_value());
    CHECK(p->string() == "/c"); // wraps to last
    CHECK(st.queue_index == 2);
}

TEST_CASE("queue_previous: no wrap in RepeatMode::None at first track") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b"}, 0); // a is current (first)
    st.repeat_mode = transporter::gui::AppState::RepeatMode::None;
    CHECK(!st.queue_previous().has_value());
    CHECK(st.queue_index == 0); // unchanged
}

TEST_CASE("queue_next: repeat-all wraps from last to first, updates index") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 2); // c is current (last)
    st.repeat_mode = transporter::gui::AppState::RepeatMode::All;
    const auto p = st.queue_next();
    REQUIRE(p.has_value());
    CHECK(p->string() == "/a");
    CHECK(st.queue_index == 0);
}

TEST_CASE("queue_next: repeat-none at last track returns nullopt") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b"}, 1); // b is current (last)
    st.repeat_mode = transporter::gui::AppState::RepeatMode::None;
    CHECK(!st.queue_next().has_value());
    CHECK(st.queue_index == 1); // unchanged
}

TEST_CASE("queue_next: repeat-one returns current, does not advance index") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1); // b is current
    st.repeat_mode = transporter::gui::AppState::RepeatMode::One;
    const auto p = st.queue_next();
    REQUIRE(p.has_value());
    CHECK(p->string() == "/b");
    CHECK(st.queue_index == 1); // unchanged
}

TEST_CASE("queue_peek_next: returns nullopt in shuffle mode (next is unpredictable)") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 0); // a is current
    st.shuffle = true;
    st.repeat_mode = transporter::gui::AppState::RepeatMode::All;
    CHECK(!st.queue_peek_next().has_value());
}

TEST_CASE("queue_move: swapping last two elements updates current") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 2); // c is current
    st.queue_move(2, 1);              // swap c up
    REQUIRE(st.queue.size() == 3);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue[1].string() == "/c");
    CHECK(st.queue[2].string() == "/b");
    CHECK(st.queue_index == 1); // c is now at index 1
}

TEST_CASE("queue_append: appends to end, does not move current index") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b"}, 0); // a is current
    st.queue_append("/c");
    REQUIRE(st.queue.size() == 3);
    CHECK(st.queue[2].string() == "/c");
    CHECK(st.queue_index == 0); // current unchanged
}

TEST_CASE("queue_insert_next: inserts after current, adjusts nothing for current") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b", "/c"}, 1); // b is current
    st.queue_insert_next("/x");
    REQUIRE(st.queue.size() == 4);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue[1].string() == "/b");
    CHECK(st.queue[2].string() == "/x"); // inserted right after current
    CHECK(st.queue[3].string() == "/c");
    CHECK(st.queue_index == 1); // current unchanged
}

TEST_CASE("queue_insert_next: empty queue appends without changing index") {
    transporter::gui::AppState st;
    // queue_index = -1 → ins = 0 → guard (ins <= 0) triggers → appends
    st.queue_insert_next("/a");
    REQUIRE(st.queue.size() == 1);
    CHECK(st.queue[0].string() == "/a");
    CHECK(st.queue_index == -1); // index not managed by insert_next
}

TEST_CASE("queue_insert_next: inserting after last element appends") {
    transporter::gui::AppState st;
    fill(st, {"/a", "/b"}, 1); // b is last and current
    st.queue_insert_next("/c");
    REQUIRE(st.queue.size() == 3);
    CHECK(st.queue[2].string() == "/c");
    CHECK(st.queue_index == 1); // current unchanged
}
