#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ExactPlanner.h"

#include <stdexcept>
#include <iostream>

namespace {

ExactScore interval(long long lower, long long upper, bool certified = false,
    bool sound = true, bool blocked = false) {
    ExactScore result;
    result.lower = ExactFraction::integer(lower);
    result.upper = ExactFraction::integer(upper);
    result.certified = certified;
    result.boundsSound = sound;
    result.certificationBlocked = blocked;
    return result;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requirePoint(const ExactScore& score, long long value, bool certified) {
    require(score.boundsSound, "merged interval is not sound");
    require(ExactCompare(score.lower, ExactFraction::integer(value)) == 0,
        "unexpected merged lower bound");
    require(ExactCompare(score.upper, ExactFraction::integer(value)) == 0,
        "unexpected merged upper bound");
    require(score.certified == certified, "unexpected certification state");
}

} // namespace

int run() {
    // A later exact slice proves an earlier wide, incomplete slice.
    const auto firstLeft = interval(0, 100);
    const auto firstRight = interval(50, 50, true);
    const auto firstMerge = mergeSoundBounds(firstLeft, firstRight);
    requirePoint(firstMerge.score,
        50, true);
    requirePoint(mergeSoundBounds(interval(50, 50, true), interval(0, 100)).score,
        50, true);

    // Neither slice is individually complete, but their sound intersection is.
    requirePoint(mergeSoundBounds(interval(0, 50), interval(50, 100)).score,
        50, true);
    requirePoint(mergeSoundBounds(interval(50, 100), interval(0, 50)).score,
        50, true);

    // An unsound estimate cannot tighten a sound interval.
    const auto unsound = mergeSoundBounds(interval(0, 100), interval(80, 80, true, false));
    require(unsound.score.boundsSound && !unsound.score.certified,
        "unsound estimate changed certification");
    require(ExactCompare(unsound.score.lower, ExactFraction::integer(0)) == 0
        && ExactCompare(unsound.score.upper, ExactFraction::integer(100)) == 0,
        "unsound estimate changed the sound interval");

    // Contradictions are explicit and never become a normal unknown score.
    const auto contradiction = mergeSoundBounds(interval(0, 40), interval(60, 100));
    require(contradiction.contradiction, "contradiction was not reported");
    require(!contradiction.score.boundsSound && contradiction.score.certificationBlocked,
        "contradiction was not fail-closed");

    // Intersections are associative for sound intervals.
    const auto left = mergeSoundBounds(
        mergeSoundBounds(interval(0, 100), interval(20, 80)).score,
        interval(40, 60)).score;
    const auto right = mergeSoundBounds(
        interval(0, 100),
        mergeSoundBounds(interval(20, 80), interval(40, 60)).score).score;
    require(ExactCompare(left.lower, right.lower) == 0
        && ExactCompare(left.upper, right.upper) == 0
        && left.certified == right.certified,
        "sound bound merge is not associative");
    return 0;
}

int main() {
    try { return run(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
