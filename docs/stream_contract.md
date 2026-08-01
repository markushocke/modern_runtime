# `modern::stream<T>` contract (R00/R01)

This document defines the first production-facing stream core. It deliberately
does not define stream algorithms, coroutine `co_yield` syntax, fan-out, or
multi-producer ordering.

## Ownership and cardinality

- `stream<T>` is a move-only, single-consumer handle.
- `stream_source<T>` is a move-only, single-producer adapter handle.
- At most one `next()` and one `send()` may be pending at a time.
- Capacity is always bounded and must be at least one. Capacity zero and
  rendezvous semantics are not part of R1.
- `make_stream()` owns producer completion: normal producer completion closes
  the stream and an uncaught producer exception fails it.

## Backpressure

`send()` completes immediately when it can hand a value directly to a waiting
reader or append it to the bounded buffer. When the buffer is full, `send()`
remains incomplete until a consumer removes a value. There is no unbounded
buffer mode.

## Terminal behavior

The lifecycle is `open`, `closing`, then `closed`, or it moves directly from
`open` to `failed` or `cancelled`.

- `close()` rejects a pending writer, drains accepted buffered values, and then
  makes `next()` return successful EOF (`std::nullopt`).
- `fail()` discards buffered values immediately. Pending and future operations
  receive the typed terminal error.
- `request_stop()` cancels the shared producer/consumer stream, discards the
  buffer, and completes pending operations with `stream_error_code::cancelled`.
- Destroying the consumer before completion behaves like shared cancellation
  with `stream_error_code::consumer_gone`.
- A consumer cancellation may replace `closing` when the close buffer has not
  yet drained. Other terminal requests do not replace an existing terminal
  decision.

Values already returned successfully from `next()` remain owned by the caller
and cannot be invalidated by a later terminal transition.

## Error channels

`next()` returns `task<stream_result<T>>`, where `stream_result<T>` is
`expected<optional<T>, stream_error>`. `send()` similarly returns a task holding
a typed expected result. Producer failure, cancellation, consumer abandonment,
and protocol failures use `stream_error`. Exceptions escaping the outer task
are reserved for failures outside the stream protocol, such as allocation
failure or a broken runtime invariant.

## Completion observers

Completion observers do not consume values and cannot change backpressure. Each
registered observer runs exactly once. A late observer is scheduled immediately
with the retained completion record. Scheduler submission failure falls back to
inline invocation so that exactly-once notification still holds. Exceptions
from observers are ignored.

For a normal close, completion is recorded after the accepted buffer has
drained and the producer task has ended. For failure and cancellation it is
recorded after pending operations have been detached and the producer has
cooperatively ended. The completion record reports accepted producer items and
values handed to consumers.

## Environment and allocation

The stream state and bounded queue use the current task environment's PMR frame
resource. Producer execution and task completions inherit the current task
environment, including scheduler, allocator, trace context, stop token, and
deadline. Higher-level start, completion, and idle timeout policy remains the
responsibility of semantic or transport invocation layers.
