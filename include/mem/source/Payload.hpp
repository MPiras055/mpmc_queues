#pragma once

namespace mem::source {

/**
 * @brief The per-thread payload of a source that nobody has asked to carry anything.
 *
 * Every source is parameterised on a `Payload` it stores in each thread's registry node, so
 * that a proxy's per-thread bookkeeping travels with the source's own and one thread-local
 * lookup serves both. A source used on its own still needs *something* to name, and this is
 * it: empty, and elided by `[[no_unique_address]]`, so it costs nothing.
 *
 * In its own header because both sources default to it, and having `Pool` include `Hazard`
 * for one empty struct made a dependency between two things that have nothing to do with
 * each other.
 */
struct NoPayload {};

} // namespace mem::source
