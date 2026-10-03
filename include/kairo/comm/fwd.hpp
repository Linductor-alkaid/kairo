#pragma once

namespace kairo::comm {

struct TopicSubscriptionOptions;
struct TopicPublishResult;

template <class T>
class MpscChannel;

template <class T>
using SpscChannel = MpscChannel<T>;

template <class T>
class LatestMailbox;

template <class T>
class RealtimeChannel;

template <class T>
class Topic;

template <class T>
class TopicSubscription;

class PhaseGate;
class Sequencer;

template <class T>
struct Snapshot;

template <class T>
class DoubleBuffer;

} // namespace kairo::comm
