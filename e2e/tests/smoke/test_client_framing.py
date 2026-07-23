from lib.nvecd_client import _complete_response_length


def test_results_header_alone_is_not_a_complete_response() -> None:
    assert _complete_response_length(b"OK RESULTS 2\r\n", "SIM item 2") is None
    complete = b"OK RESULTS 2\r\nfirst 0.9\r\nsecond 0.8\r\n"
    assert _complete_response_length(complete, "SIM item 2") == len(complete)


def test_end_terminated_response_waits_for_end_marker() -> None:
    assert _complete_response_length(b"# Server\r\nversion:0.1\r\n", "INFO") is None
    complete = b"# Server\r\nversion:0.1\r\nEND\r\n"
    assert _complete_response_length(complete, "INFO") == len(complete)
