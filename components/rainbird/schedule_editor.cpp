#include "schedule_editor.h"

#include <cstring>

namespace esphome::rainbird {

bool ScheduleEditor::start(const PageChange &change, ByteBuf *read) {
  std::optional<PageField> located = this->model_.locate(change.field, change.zone, change.program, change.index);
  if (!located.has_value() || read == nullptr) {
    return false;
  }
  this->active_ = true;
  this->phase_ = Phase::READING;
  this->changes_[0] = change;
  this->count_ = 1;
  this->refused_ = 0;
  this->page_ = located->page;
  this->reason_ = nullptr;
  *read = request_read_page(this->page_);
  return true;
}

bool ScheduleEditor::add(const PageChange &change) {
  // Once the read is answered the page is patched and on its way; a later change needs its own read.
  if (!this->active_ || this->phase_ != Phase::READING) {
    return false;
  }
  std::optional<PageField> located = this->model_.locate(change.field, change.zone, change.program, change.index);
  if (!located.has_value() || located->page != this->page_) {
    return false;
  }
  // The same field again: the later value is the one wanted, and it is the latest thing asked.
  for (size_t i = 0; i < this->count_; i++) {
    if (this->changes_[i].same_field(change)) {
      for (size_t j = i; j + 1 < this->count_; j++) {
        this->changes_[j] = this->changes_[j + 1];
      }
      this->count_--;
      break;
    }
  }
  if (this->count_ >= MAX_CHANGES) {
    return false;
  }
  this->changes_[this->count_++] = change;
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
  // Each change is tried on a copy, so one the page will not take leaves the others' bytes alone.
  uint8_t patched[MAX_PAGE_BYTES];
  std::memcpy(patched, page.data, page.size);
  size_t out_size = page.size;
  size_t applied = 0;
  for (size_t i = 0; i < this->count_; i++) {
    const PageChange &change = this->changes_[i];
    uint8_t tried[MAX_PAGE_BYTES];
    size_t tried_size = sizeof(tried);
    if (this->model_.patch(this->page_, patched, page.size, change.field, change.zone, change.program, change.index,
                           change.value, tried, &tried_size)) {
      std::memcpy(patched, tried, tried_size);
      applied++;
    }
  }
  this->refused_ = this->count_ - applied;
  if (applied == 0) {
    return this->fail_("that field could not be changed on the page");
  }
  // Nothing to write. The read that just came back is already what the entities show.
  if (std::memcmp(patched, page.data, page.size) == 0) {
    this->active_ = false;
    return Step::UNCHANGED;
  }
  if (!request_write_page(this->page_, patched, out_size, next)) {
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
