from importlib import util
from pathlib import Path

_TOOL = Path(__file__).resolve().parents[2] / "tools" / "raw_to_fixture.py"
_spec = util.spec_from_file_location("raw_to_fixture", _TOOL)
raw_to_fixture = util.module_from_spec(_spec)
_spec.loader.exec_module(raw_to_fixture)


def test_dump_lines_and_continuations_join():
    log = [
        "\x1b[0;32m[10:00:00][I][remote.raw:041]: Received Raw: 208, -208, 416\x1b[0m",
        "[10:00:00][I][remote.raw:041]:   -416, 208, -4000",
        "[10:00:00][D][rainbird:044]: Receiver 92 13 A7 EA",
        "[10:00:01][I][remote.raw:041]: Received Raw: 300, -4000",
    ]
    assert raw_to_fixture.parse_log(log) == [[208, -208, 416, -416, 208, -4000], [300, -4000]]


def test_tag_digits_are_not_timings():
    # "remote.raw:041" carries a line number; only the text after "]:" is data.
    log = ["[I][remote.raw:041]: Received Raw: 208", "[I][remote.raw:041]:   -208"]
    assert raw_to_fixture.parse_log(log) == [[208, -208]]


def test_a_continuation_must_follow_its_dump():
    log = [
        "[I][remote.raw:041]: Received Raw: 208",
        "[I][rainbird:044]: something else",
        "[I][remote.raw:041]:   -208",
    ]
    assert raw_to_fixture.parse_log(log) == [[208]]


def test_render_writes_note_receiver_and_bursts():
    text = raw_to_fixture.render([[208, -4000]], "first line\nsecond", "rx")
    assert text == "# first line\n# second\nreceiver: rx\nburst: 208, -4000\n"
