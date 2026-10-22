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
