#include "schedule_editor.h"

#include <cstring>

namespace esphome::rainbird {

bool ScheduleEditor::start(Field field, uint8_t zone, uint8_t program, uint8_t index, int32_t value, ByteBuf *read) {
  std::optional<PageField> located = this->model_.locate(field, zone, program, index);
  if (!located.has_value() || read == nullptr) {
    return false;
  }
  this->active_ = true;
  this->phase_ = Phase::READING;
  this->field_ = field;
  this->zone_ = zone;
  this->program_ = program;
  this->index_ = index;
  this->value_ = value;
  this->page_ = located->page;
  this->reason_ = nullptr;
  *read = request_read_page(this->page_);
  return true;
}

ScheduleEditor::Step ScheduleEditor::fail_(const char *reason) {
  this->active_ = false;
  this->reason_ = reason;
  return Step::FAILED;
}

ScheduleEditor::Step ScheduleEditor::on_reply(const Completion &done, ByteBuf *next) {
  if (!this->active_ || next == nullptr) {
    return this->fail_("no schedule edit was waiting for that reply");
  }
  if (done.outcome != Outcome::REPLY) {
    return this->fail_("the page read or write was not acknowledged");
  }
  if (this->phase_ == Phase::WRITING) {
    // The link matched this reply to the write, so an ACK here echoes 21.
    if (done.reply.kind != ReplyKind::ACK) {
      return this->fail_("the page write was not acknowledged");
    }
    *next = request_read_page(this->page_);
    this->phase_ = Phase::CONFIRMING;
    return Step::CONFIRM;
  }
  // Reading and confirming work from this reply's page. Any other answer, an ACK for one, is not the page.
  if (done.reply.kind != ReplyKind::SCHEDULE || done.reply.schedule.page != this->page_) {
    return this->fail_("the page read did not return that page");
  }
  const SchedulePage &page = done.reply.schedule;
  if (this->phase_ == Phase::CONFIRMING) {
    if (page.size != this->log_.size || std::memcmp(page.data, this->log_.after, page.size) != 0) {
      return this->fail_("the confirming read does not match the page that was written");
    }
    this->active_ = false;
    return Step::DONE;
  }
  uint8_t patched[MAX_PAGE_BYTES];
  size_t out_size = page.size;
  if (!this->model_.patch(this->page_, page.data, page.size, this->field_, this->zone_, this->program_, this->index_,
                          this->value_, patched, &out_size) ||
      !request_write_page(this->page_, patched, out_size, next)) {
    return this->fail_("that field could not be changed on the page");
  }
  this->log_.page = this->page_;
  this->log_.size = static_cast<uint8_t>(out_size);
  std::memcpy(this->log_.before, page.data, out_size);
  std::memcpy(this->log_.after, patched, out_size);
  this->phase_ = Phase::WRITING;
  return Step::WRITE;
}

}  // namespace esphome::rainbird
