#include "doctest.h"

#include "fake_controller.h"
#include "hex.h"
#include "link_core.h"

#include <vector>

using namespace esphome::rainbird;

static ByteBuf buf(const char *hex) {
  ByteBuf out;
  for (uint8_t byte : from_hex(hex)) {
    out.push(byte);
  }
  return out;
}

static void reply(LinkCore &link, const char *hex, uint32_t now) {
  auto bytes = from_hex(hex);
  link.on_frame(bytes.data(), bytes.size(), now);
}

static std::string sent(const Transmission &tx) {
  auto payload = first_payload(tx);
  return to_hex(payload.data(), payload.size());
}

TEST_CASE("air time is 5/12 ms per bit, rounded up") {
  CHECK(frame_air_ms(1) == 4);   // 8 bits = 3.33 ms
  CHECK(frame_air_ms(8) == 27);  // a 2-byte request frame
  CHECK(frame_air_ms(42) == 140);
}

TEST_CASE("2-byte requests go out twice with no gap, others once") {
  LinkCore link;
  REQUIRE(link.enqueue(buf("3F 00"), Priority::POLL, 1));
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  CHECK(tx.size == 16);
  CHECK(to_hex(tx.bytes, 8) == to_hex(tx.bytes + 8, 8));
  CHECK(tx.first_copy_ms == 27);
  CHECK(tx.total_ms == 54);

  LinkCore single;
  REQUIRE(single.enqueue(buf("4C"), Priority::POLL, 1));
  REQUIRE(single.next_transmission(0, &tx));
  CHECK(tx.size == 7);

  LinkTiming plain;
  plain.repeat_short = false;
  LinkCore once(plain);
  REQUIRE(once.enqueue(buf("3F 00"), Priority::POLL, 1));
  REQUIRE(once.next_transmission(0, &tx));
  CHECK(tx.size == 8);
}

TEST_CASE("one request in flight, highest priority first, then oldest") {
  LinkCore link;
  link.enqueue(buf("3F 00"), Priority::POLL, 1);
  link.enqueue(buf("40"), Priority::USER, 2);
  link.enqueue(buf("0B"), Priority::FOLLOW_UP, 3);
  link.enqueue(buf("4C"), Priority::POLL, 4);
  Transmission tx;
  uint32_t now = 0;
  std::vector<std::string> order;
  const char *answers[] = {"01 40", "8B 01 2F 00 00", "BF 00 00 00 00 00", "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00"};
  for (const char *answer : answers) {
    REQUIRE(link.next_transmission(now, &tx));
    order.push_back(sent(tx));
    // Nothing else goes out while this one waits.
    Transmission blocked;
    CHECK_FALSE(link.next_transmission(now + 1, &blocked));
    now += tx.total_ms + 30;
    reply(link, answer, now);
    now += 100;
  }
  CHECK(order == std::vector<std::string>{"40", "0B", "3F 00", "4C"});
  CHECK(link.idle() == false);  // completions not collected yet
  Completion done;
  int count = 0;
  while (link.pop_completion(&done)) {
    count++;
  }
  CHECK(count == 4);
  CHECK(link.idle());
}

TEST_CASE("the timeout runs from the end of the first copy, then retries after the gap") {
  LinkCore link;  // 400 ms timeout, 2 retries, 20 ms gap
  link.enqueue(buf("3F 00"), Priority::POLL, 7);
  Transmission tx;
  REQUIRE(link.next_transmission(1000, &tx));
  uint32_t deadline = 1000 + tx.first_copy_ms + 400;
  link.tick(deadline - 1);
  CHECK(link.in_flight());
  link.tick(deadline);
  CHECK_FALSE(link.in_flight());
  CHECK(link.link_errors() == 1);
  CHECK_FALSE(link.next_transmission(deadline + 19, &tx));
  REQUIRE(link.next_transmission(deadline + 20, &tx));
  CHECK(sent(tx) == "3F 00");

  // Third attempt fails for good.
  uint32_t now = deadline + 20;
  link.tick(now + tx.first_copy_ms + 400);
  now += tx.first_copy_ms + 400 + 20;
  REQUIRE(link.next_transmission(now, &tx));
  link.tick(now + tx.first_copy_ms + 400);
  Completion done;
  REQUIRE(link.pop_completion(&done));
  CHECK(done.outcome == Outcome::FAILED);
  CHECK(done.tag == 7);
  CHECK(done.attempts == 3);
  CHECK(link.link_errors() == 3);
  CHECK(link.consecutive_failures() == 1);
}

TEST_CASE("replies must match the request's opcode and selector") {
  LinkCore link;
  link.enqueue(buf("3B 01"), Priority::POLL, 1);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  reply(link, "BB 00 00 00 00 00 00 00 00 FF 00 00", 60);  // a late page-0 reply
  CHECK(link.in_flight());
  CHECK(link.unexpected() == 1);
  reply(link, "BB 01 21 05 D0 00 00 00", 70);
  CHECK_FALSE(link.in_flight());
  Completion done;
  REQUIRE(link.pop_completion(&done));
  CHECK(done.outcome == Outcome::REPLY);
  CHECK(done.reply.kind == ReplyKind::QUEUE_LIST);
  CHECK(done.response_ms == 70 - tx.first_copy_ms);

  CHECK(reply_matches(from_hex("20 00 80").data(), 3, from_hex("A0 00 80 00").data(), 4));
  CHECK_FALSE(reply_matches(from_hex("20 00 80").data(), 3, from_hex("A0 00 81 00").data(), 4));
  CHECK(reply_matches(from_hex("39 00 01 06").data(), 4, from_hex("01 39").data(), 2));
  CHECK_FALSE(reply_matches(from_hex("39 00 01 06").data(), 4, from_hex("01 40").data(), 2));
  CHECK(reply_matches(from_hex("49 01").data(), 2, from_hex("00 49 01").data(), 3));
}

TEST_CASE("a NAK answers the request and is not retried") {
  LinkCore link;
  link.enqueue(buf("49 01"), Priority::USER, 9);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  reply(link, "00 49 01", 60);
  Completion done;
  REQUIRE(link.pop_completion(&done));
  CHECK(done.outcome == Outcome::NAK);
  CHECK(done.reply.nak.code == 0x01);
  CHECK(link.link_errors() == 0);
  CHECK(link.queued() == 0);
}

TEST_CASE("a second identical reply is counted as a duplicate, not a new answer") {
  LinkCore link;
  link.enqueue(buf("3F 00"), Priority::POLL, 1);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  reply(link, "BF 00 01 00 00 00", 57);
  reply(link, "BF 00 01 00 00 00", 84);
  CHECK(link.duplicates() == 1);
  CHECK(link.unexpected() == 0);
}

TEST_CASE("the next request waits for our own duplicate copy to finish") {
  LinkCore link;
  link.enqueue(buf("3F 00"), Priority::POLL, 1);
  link.enqueue(buf("4C"), Priority::POLL, 2);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  REQUIRE(tx.total_ms == 54);
  // An answer that lands while the second copy is still going out (at 37 of 54 ms).
  reply(link, "BF 00 01 00 00 00", tx.first_copy_ms + 10);
  Transmission next;
  CHECK_FALSE(link.next_transmission(54 + 19, &next));
  CHECK(link.next_transmission(54 + 20, &next));
}

TEST_CASE("an answer after both copies only waits the gap") {
  LinkCore link;
  link.enqueue(buf("3F 00"), Priority::POLL, 1);
  link.enqueue(buf("4C"), Priority::POLL, 2);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  reply(link, "BF 00 01 00 00 00", 57);  // ~30 ms after the first copy, as captured
  Transmission next;
  CHECK_FALSE(link.next_transmission(57 + 19, &next));
  CHECK(link.next_transmission(57 + 20, &next));
}

TEST_CASE("a reply with the right opcode and the wrong length leaves the request waiting") {
  LinkCore link;
  link.enqueue(buf("4C"), Priority::POLL, 1);
  Transmission tx;
  REQUIRE(link.next_transmission(0, &tx));
  reply(link, "CC 14 0B", 40);
  CHECK(link.in_flight());
  CHECK(link.unexpected() == 1);
}

TEST_CASE("clock wrap and long idle spells do not block or break timeouts") {
  LinkCore link;
  link.enqueue(buf("4C"), Priority::POLL, 1);
  Transmission tx;
  // First request ever, more than 24.8 days after boot.
  uint32_t start = 0xFFFFFF00u;
  REQUIRE(link.next_transmission(start, &tx));
  link.tick(start + 100);
  CHECK(link.in_flight());
  link.tick(start + tx.first_copy_ms + 400);  // wraps past zero
  CHECK_FALSE(link.in_flight());

  // A month of silence later, the next request still goes straight out.
  LinkCore idle;
  idle.enqueue(buf("4C"), Priority::POLL, 1);
  REQUIRE(idle.next_transmission(0, &tx));
  reply(idle, "CC 14 0B 23 08 17 E7 00 03 00 01 FF FF 00 00 00", 40);
  idle.enqueue(buf("4C"), Priority::POLL, 2);
  CHECK(idle.next_transmission(40 + 2592000000u, &tx));
}
