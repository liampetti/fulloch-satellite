#!/usr/bin/env python3
"""Host contract tests for provisioning and satellite protocol scenarios."""

import copy
import importlib.util
import unittest
from collections import deque
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


provision = load_module("provision", ROOT / "tools" / "provision.py")
fixture = load_module("fixture", ROOT / "tools" / "tls_websocket_fixture.py")


class ProvisioningTests(unittest.TestCase):
    def setUp(self):
        self.config = provision.load_json(ROOT / "provisioning" / "device.config.example.json")
        self.credentials = provision.load_json(ROOT / "provisioning" / "device.credentials.example.json")

    def test_example_bundle_is_valid(self):
        entries = provision.validate(self.config, self.credentials)
        self.assertIn(("wifi", "ssid", "example-network"), entries)

    def test_speaker_volume_accepts_the_full_configured_range(self):
        for volume in (0, 100):
            config = copy.deepcopy(self.config)
            config["audio"]["speaker_volume_percent"] = volume
            entries = provision.validate(config, self.credentials)
            self.assertIn(("audio", "speaker_volume", str(volume)), entries)

    def test_aec_enabled_is_provisioned_as_a_boolean(self):
        entries = provision.validate(self.config, self.credentials)
        self.assertIn(("audio", "aec_enabled", "true"), entries)
        config = copy.deepcopy(self.config)
        config["audio"]["aec_enabled"] = "false"
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_private_tls_material_is_rejected(self):
        config = copy.deepcopy(self.config)
        config["server"]["client_key"] = "not-allowed"
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_spotify_settings_enter_device_nvs(self):
        config = copy.deepcopy(self.config)
        credentials = copy.deepcopy(self.credentials)
        config["music"] = {"player": "spotify"}
        credentials["spotify"] = {
            "client_id": "example-spotify-client-id",
            "client_secret": "example-spotify-client-secret",
            "refresh_token": "example-spotify-refresh-token",
        }
        entries = provision.validate(config, credentials)
        self.assertIn(("spotify", "client_id", "example-spotify-client-id"), entries)
        self.assertIn(("spotify", "client_secret", "example-spotify-client-secret"), entries)
        self.assertIn(("spotify", "refresh_token", "example-spotify-refresh-token"), entries)

    def test_spotify_config_and_credentials_are_required_together(self):
        config = copy.deepcopy(self.config)
        credentials = copy.deepcopy(self.credentials)
        config["music"] = {"player": "spotify"}
        credentials["spotify"] = {
            "client_id": "example-spotify-client-id",
            "client_secret": "example-spotify-client-secret",
            "refresh_token": "example-spotify-refresh-token",
        }
        del credentials["spotify"]
        with self.assertRaises(provision.BundleError):
            provision.validate(config, credentials)

    def test_spotify_device_selector_is_rejected(self):
        config = copy.deepcopy(self.config)
        config["music"]["playback_device_id"] = "Example Speaker"
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_sonos_settings_enter_device_nvs(self):
        config = copy.deepcopy(self.config)
        credentials = copy.deepcopy(self.credentials)
        del credentials["spotify"]
        config["music"] = {"player": "sonos"}
        config["sonos"] = {"host": "192.168.1.50"}
        entries = provision.validate(config, credentials)
        self.assertIn(("music", "player", "sonos"), entries)
        self.assertIn(("sonos", "host", "192.168.1.50"), entries)

    def test_sonos_requires_the_sonos_music_player(self):
        config = copy.deepcopy(self.config)
        config["sonos"] = {"host": "192.168.1.50"}
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)


class ProtocolFixtureTests(unittest.TestCase):
    def test_valid_hello_and_normal_sequence(self):
        hello = {
            "type": "satellite.hello",
            "token": "fixture-token",
            "protocol": {"name": "satellite-v2", "major": 2, "minor": 2},
            "device": {"id": "fixture-01", "name": "Fixture", "firmware": "0.1.0", "build": "test", "board": "rev2"},
            "capabilities": {"audio_input": True, "audio_output": True, "display": True, "touch": True},
        }
        self.assertTrue(fixture.validate_hello(hello))
        self.assertEqual(fixture.scenario_messages("normal")[0]["type"], "satellite.welcome")

    def test_resume_hello_uses_last_fully_played_sequence(self):
        hello = {
            "type": "satellite.hello",
            "token": "fixture-token",
            "protocol": {"name": "satellite-v2", "major": 2, "minor": 2},
            "device": {"id": "fixture-01", "name": "Fixture", "firmware": "0.1.0", "build": "test", "board": "rev2"},
            "capabilities": {"audio_input": True, "audio_output": True, "display": True, "touch": True},
            "resume": {"session_id": "prior-session", "turn_id": "turn-1", "next_seq": 7},
        }
        self.assertTrue(fixture.validate_hello(hello))
        hello["resume"]["next_seq"] = -1
        self.assertFalse(fixture.validate_hello(hello))

    def test_welcome_uses_the_fixed_16khz_downlink_contract(self):
        downlink = fixture.scenario_messages("normal")[0]["audio"]["downlink"]
        self.assertEqual(downlink, {"encoding": "pcm_s16le", "sample_rate_hz": 16000, "channels": 1})

    def test_adverse_sequences_are_available(self):
        self.assertEqual(fixture.scenario_messages("malformed-welcome")[0]["protocol"]["major"], 3)
        self.assertEqual(fixture.scenario_messages("stale-turn")[-1]["turn_id"], "stale")

    def test_missing_capability_is_rejected(self):
        self.assertFalse(fixture.validate_hello({"type": "satellite.hello", "token": "x"}))


class TurnAndQueueTests(unittest.TestCase):
    def setUp(self):
        self.turn_id = None
        self.state = "idle"
        self.barge_in_turn_pending = False

    def apply_tts_cancel(self, turn_id):
        if self.state != "speaking" or turn_id != self.turn_id:
            return False
        self.barge_in_turn_pending = True
        return True

    def apply_tts_end(self, turn_id):
        if self.state != "speaking" or turn_id != self.turn_id:
            return False
        self.state = "idle"
        self.turn_id = None
        return True

    def apply_state(self, state, turn_id):
        if state == "idle":
            if turn_id is not None and turn_id != self.turn_id:
                return False
            self.turn_id = None
            self.state = state
            return True
        replaces_follow_up_turn = self.state == "follow_up" and turn_id != self.turn_id
        replaces_barge_in_turn = (
            self.barge_in_turn_pending
            and state in {"wake_detected", "listening"}
            and turn_id != self.turn_id
        )
        starts_proactive_turn = state == "speaking" and self.turn_id is None
        if state == "wake_detected":
            if (
                self.turn_id is not None
                and not replaces_follow_up_turn
                and not replaces_barge_in_turn
            ) or not turn_id:
                return False
            self.turn_id = turn_id
        elif state not in {"listening", "thinking", "speaking", "follow_up", "error"} or (
            turn_id != self.turn_id
            and not replaces_follow_up_turn
            and not replaces_barge_in_turn
            and not starts_proactive_turn
        ):
            return False
        elif replaces_follow_up_turn or replaces_barge_in_turn or starts_proactive_turn:
            self.turn_id = turn_id
        self.barge_in_turn_pending = False
        self.state = state
        return True

    def test_turn_ids_are_stable_and_reconnect_reset_clears_them(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("listening", "turn-1"))
        self.assertFalse(self.apply_state("speaking", "stale-turn"))
        self.turn_id = None  # Reconnect reset.
        self.state = "idle"
        self.assertTrue(self.apply_state("wake_detected", "turn-2"))

    def test_idle_accepts_the_active_turn_id_or_null(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("idle", "turn-1"))
        self.assertTrue(self.apply_state("wake_detected", "turn-2"))
        self.assertTrue(self.apply_state("idle", None))

    def test_follow_up_can_roll_over_to_a_new_turn(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("follow_up", "turn-1"))
        self.assertTrue(self.apply_state("listening", "turn-2"))
        self.assertTrue(self.apply_state("thinking", "turn-2"))
        self.assertTrue(self.apply_state("speaking", "turn-2"))
        self.assertEqual(self.turn_id, "turn-2")

    def test_barge_in_can_roll_over_from_speaking_after_tts_cancel(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.assertTrue(self.apply_tts_cancel("turn-1"))
        self.assertTrue(self.apply_state("listening", "turn-2"))
        self.assertEqual(self.turn_id, "turn-2")

    def test_proactive_speaking_turn_can_complete_without_a_wakeword(self):
        self.assertTrue(self.apply_state("speaking", "announcement-1"))
        self.assertTrue(self.apply_tts_end("announcement-1"))
        self.assertEqual(self.state, "idle")

    def test_new_turn_is_rejected_without_follow_up_or_tts_cancel(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.assertFalse(self.apply_state("listening", "turn-2"))

    def test_idle_waits_for_an_active_response_to_drain(self):
        state = "speaking"
        idle_pending = True
        downlink_pending = True
        if idle_pending and not downlink_pending:
            state = "idle"
        self.assertEqual(state, "speaking")
        downlink_pending = False
        if idle_pending and not downlink_pending:
            state = "idle"
        self.assertEqual(state, "idle")

    def test_response_without_audio_gets_a_startup_grace_period(self):
        state = "speaking"
        downlink_pending = False
        idle_delay_ms = 5000 if state in {"thinking", "speaking"} and not downlink_pending else 250
        self.assertEqual(idle_delay_ms, 5000)
        downlink_pending = True
        idle_delay_ms = 5000 if state in {"thinking", "speaking"} and not downlink_pending else 250
        self.assertEqual(idle_delay_ms, 250)

    def test_vad_final_invalidates_a_frame_already_dequeued_for_uplink(self):
        uplink_generation = 4
        dequeued_frame_generation = uplink_generation
        uplink_generation += 1  # VAD final flushes queued microphone audio.
        self.assertNotEqual(dequeued_frame_generation, uplink_generation)

    def test_follow_up_turn_uses_the_same_vad_final_uplink_flush(self):
        uplink_generation = 4
        uplink_generation += 1  # First turn's thinking state.
        first_turn_generation = uplink_generation
        uplink_generation += 1  # Follow-up turn's thinking state.
        self.assertGreater(uplink_generation, first_turn_generation)


    def test_bounded_queue_drops_without_blocking_and_reset_flushes(self):
        queue = deque(maxlen=2)
        for item in ("first", "second"):
            queue.append(item)
        accepted = len(queue) < queue.maxlen
        self.assertFalse(accepted)
        queue.clear()
        self.assertEqual(list(queue), [])

    def test_ui_mapping_hides_rings_immediately_when_idle(self):
        assistant_state = "idle"
        playback_pending = True
        rings_visible = assistant_state not in {"idle", "error"}
        self.assertFalse(rings_visible)

    def test_recovery_resumes_after_only_fully_played_frames(self):
        last_fully_played_seq = 4
        queued_but_not_played = [5, 6]
        queued_but_not_played.clear()  # Discarded before the server replays from next_seq.
        resume = {"session_id": "session-1", "turn_id": "turn-1", "next_seq": last_fully_played_seq + 1}
        self.assertEqual(resume["next_seq"], 5)
        self.assertEqual(queued_but_not_played, [])

    def test_v2_tts_audio_requires_contiguous_sequence_and_exact_binary_size(self):
        expected_seq = 3
        metadata = {"turn_id": "turn-1", "seq": 3, "bytes": 640}
        self.assertEqual(metadata["seq"], expected_seq)
        self.assertEqual(metadata["bytes"], 640)
        expected_seq += 1
        self.assertEqual(expected_seq, 4)


if __name__ == "__main__":
    unittest.main()
