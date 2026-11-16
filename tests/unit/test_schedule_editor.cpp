#include "doctest.h"

#include "hex.h"
#include "model_esp_tm2.h"
#include "schedule_editor.h"

#include <string>

using namespace esphome::rainbird;

static Completion answer(const char *hex) {
  Completion done;
  auto bytes = from_hex(hex);
  REQUIRE(parse_reply(bytes.data(), bytes.size(), &done.reply));
  done.outcome = done.reply.kind == ReplyKind::NAK ? Outcome::NAK : Outcome::REPLY;
  return done;
}

TEST_CASE("an edit reads, writes one field, and confirms by reading back") {
  EspTm2Model model;
  ScheduleEditor editor(model);
  ByteBuf request;
  REQUIRE(editor.start(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, &request));
  CHECK(to_hex(request.data, request.size) == "20 00 10");

  REQUIRE(editor.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::WRITE);
  CHECK(to_hex(request.data, request.size) == "21 00 10 6A 06 01 00 64 00");
  CHECK(editor.write_log().before[5] == FREQ_CYCLIC);
  CHECK(editor.write_log().after[5] == FREQ_CUSTOM);

  REQUIRE(editor.on_reply(answer("01 21"), &request) == ScheduleEditor::Step::CONFIRM);
  CHECK(to_hex(request.data, request.size) == "20 00 10");
  CHECK(editor.on_reply(answer("A0 00 10 6A 06 01 00 64 00"), &request) == ScheduleEditor::Step::DONE);
  CHECK_FALSE(editor.active());
}

TEST_CASE("an edit fails on any answer that is not the step's own") {
  EspTm2Model model;
  ByteBuf request;

  ScheduleEditor ack_for_page(model);
  REQUIRE(ack_for_page.start(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, &request));
  CHECK(ack_for_page.on_reply(answer("01 20"), &request) == ScheduleEditor::Step::FAILED);
  CHECK_FALSE(ack_for_page.active());

  ScheduleEditor other_page(model);
  REQUIRE(other_page.start(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, &request));
  CHECK(other_page.on_reply(answer("A0 00 11 7F 03 00 00 2D 00"), &request) == ScheduleEditor::Step::FAILED);

  ScheduleEditor changed_back(model);
  REQUIRE(changed_back.start(Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM, &request));
  REQUIRE(changed_back.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::WRITE);
  REQUIRE(changed_back.on_reply(answer("01 21"), &request) == ScheduleEditor::Step::CONFIRM);
  CHECK(changed_back.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::FAILED);
  CHECK(std::string(changed_back.reason()).find("does not match") != std::string::npos);

  ScheduleEditor nowhere(model);
  CHECK_FALSE(nowhere.start(Field::RUN_TIME, 13, 0, 0, 10, &request));
}

TEST_CASE("changes to one page share its read and its write, in the order asked") {
  EspTm2Model model;
  ScheduleEditor editor(model);
  ByteBuf request;
  // 6A is Monday, Wednesday, Friday and Saturday.
  REQUIRE(editor.start(Field::WATERING_DAY, 0, 0, 0, 0, &request));
  CHECK(editor.add(PageChange{Field::FREQUENCY, 0, 0, 0, FREQ_CUSTOM}));
  CHECK(editor.add(PageChange{Field::CYCLE_DAYS, 0, 0, 0, 3}));
  // The frequency again: the later value wins.
  CHECK(editor.add(PageChange{Field::FREQUENCY, 0, 0, 0, FREQ_EVEN}));
  // Another program's page, and a field that is on no page, wait for their own edit.
  CHECK_FALSE(editor.add(PageChange{Field::FREQUENCY, 0, 1, 0, FREQ_EVEN}));
  CHECK_FALSE(editor.add(PageChange{Field::RUN_TIME, 13, 0, 0, 10}));

  REQUIRE(editor.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::WRITE);
  CHECK(to_hex(request.data, request.size) == "21 00 10 68 03 01 00 64 03");
  CHECK(editor.refused() == 0);
  // The page is patched. A change now would miss the write.
  CHECK_FALSE(editor.add(PageChange{Field::WATERING_DAY, 0, 0, 2, 0}));
}

TEST_CASE("a start slot is turned off then given a time, or the other way round, as asked") {
  EspTm2Model model;
  ByteBuf request;
  ScheduleEditor on(model);
  REQUIRE(on.start(Field::START_ENABLED, 0, 0, 0, 0, &request));
  REQUIRE(on.add(PageChange{Field::START_TIME, 0, 0, 0, 400}));
  REQUIRE(on.on_reply(answer("A0 00 60 01 68 FF FF FF FF FF FF"), &request) == ScheduleEditor::Step::WRITE);
  CHECK(to_hex(request.data, request.size) == "21 00 60 01 90 FF FF FF FF FF FF");

  ScheduleEditor off(model);
  REQUIRE(off.start(Field::START_TIME, 0, 0, 0, 400, &request));
  REQUIRE(off.add(PageChange{Field::START_ENABLED, 0, 0, 0, 0}));
  REQUIRE(off.on_reply(answer("A0 00 60 01 68 FF FF FF FF FF FF"), &request) == ScheduleEditor::Step::WRITE);
  CHECK(to_hex(request.data, request.size) == "21 00 60 FF FF FF FF FF FF FF FF");
}

TEST_CASE("a page that already holds every value is not written") {
  EspTm2Model model;
  ScheduleEditor editor(model);
  ByteBuf request;
  REQUIRE(editor.start(Field::FREQUENCY, 0, 0, 0, FREQ_CYCLIC, &request));
  REQUIRE(editor.add(PageChange{Field::WATERING_DAY, 0, 0, 0, 1}));
  CHECK(editor.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::UNCHANGED);
  CHECK_FALSE(editor.active());
}

TEST_CASE("a change the page will not take is counted, and the others still go") {
  EspTm2Model model;
  ByteBuf request;
  ScheduleEditor editor(model);
  REQUIRE(editor.start(Field::CYCLE_DAYS, 0, 0, 0, 300, &request));  // one byte cannot hold 300
  REQUIRE(editor.add(PageChange{Field::FREQUENCY, 0, 0, 0, FREQ_ODD}));
  REQUIRE(editor.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::WRITE);
  CHECK(to_hex(request.data, request.size) == "21 00 10 6A 06 01 00 64 02");
  CHECK(editor.refused() == 1);

  // With nothing left to change, the edit fails and writes nothing.
  ScheduleEditor alone(model);
  REQUIRE(alone.start(Field::CYCLE_DAYS, 0, 0, 0, 300, &request));
  CHECK(alone.on_reply(answer("A0 00 10 6A 06 01 00 64 01"), &request) == ScheduleEditor::Step::FAILED);
  CHECK(alone.refused() == 1);
}
