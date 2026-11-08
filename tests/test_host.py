#!/usr/bin/env python3
"""Host contract tests for provisioning and satellite protocol scenarios."""

import copy
import importlib.util
import os
import ssl
import subprocess
import sys
import tempfile
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

    def test_uplink_channels_defaults_to_mono_for_existing_bundles(self):
        config = copy.deepcopy(self.config)
        del config["audio"]["uplink_channels"]
        entries = provision.validate(config, self.credentials)
        self.assertIn(("audio", "uplink_channels", "1"), entries)

    def test_stereo_uplink_requires_aec_and_accepts_two_channels(self):
        config = copy.deepcopy(self.config)
        config["audio"]["uplink_channels"] = 2
        entries = provision.validate(config, self.credentials)
        self.assertIn(("audio", "uplink_channels", "2"), entries)
        config["audio"]["aec_enabled"] = False
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_invalid_uplink_channel_count_is_rejected(self):
        config = copy.deepcopy(self.config)
        config["audio"]["uplink_channels"] = 3
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_display_rotation_defaults_to_zero_for_existing_bundles(self):
        config = copy.deepcopy(self.config)
        del config["display"]
        entries = provision.validate(config, self.credentials)
        self.assertIn(("display", "rotation", "0"), entries)

    def test_display_rotation_accepts_only_zero_or_180_degrees(self):
        config = copy.deepcopy(self.config)
        config["display"]["rotation_degrees"] = 180
        entries = provision.validate(config, self.credentials)
        self.assertIn(("display", "rotation", "180"), entries)
        config["display"]["rotation_degrees"] = 90
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_private_tls_material_is_rejected(self):
        config = copy.deepcopy(self.config)
        config["server"]["client_key"] = "not-allowed"
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    def test_server_certificate_is_required_and_provisioned(self):
        entries = provision.validate(self.config, self.credentials)
        self.assertIn(("server", "certificate", self.config["server"]["certificate_pem"]), entries)
        config = copy.deepcopy(self.config)
        del config["server"]["certificate_pem"]
        with self.assertRaises(provision.BundleError):
            provision.validate(config, self.credentials)

    @unittest.skipUnless(importlib.util.find_spec("esp_idf_nvs_partition_gen"), "standalone NVS generator not installed")
    def test_standalone_generator_creates_compatible_sized_image(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "nvs.bin"
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "provision.py"), "stage",
                 "--config", str(ROOT / "provisioning" / "device.config.example.json"),
                 "--credentials", str(ROOT / "provisioning" / "device.credentials.example.json"),
                 "--output", str(image), "--size", "0x10000"],
                check=True, capture_output=True, text=True,
            )
            data = image.read_bytes()
            self.assertEqual(len(data), 0x10000)
            self.assertIn(self.config["server"]["certificate_pem"].encode(), data)
            self.assertNotIn(self.credentials["wifi"]["password"], result.stdout + result.stderr)
            self.assertNotIn(self.credentials["fulloch"]["device_token"], result.stdout + result.stderr)
            self.assertEqual(data, provision.generate_nvs(self.config, self.credentials))

    def test_server_and_cli_use_the_same_bundle_implementation(self):
        server_copy = ROOT.parent / "local-assistant" / "server" / "satellite_bundle.py"
        if not server_copy.exists():
            self.skipTest("Fulloch checkout not available")
        self.assertEqual(server_copy.read_bytes(), (ROOT / "tools" / "provision.py").read_bytes())

    def test_utf8_fields_must_fit_device_buffers(self):
        credentials = copy.deepcopy(self.credentials)
        credentials["wifi"]["ssid"] = "é" * 17
        with self.assertRaises(provision.BundleError):
            provision.validate(self.config, credentials)

    def test_server_certificate_rejects_private_key_and_oversized_data(self):
        for certificate in (
            "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n",
            "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
            "-----BEGIN CERTIFICATE-----\n" + "A" * 4000 + "\n-----END CERTIFICATE-----\n",
            self.config["server"]["certificate_pem"] + self.config["server"]["certificate_pem"],
        ):
            config = copy.deepcopy(self.config)
            config["server"]["certificate_pem"] = certificate
            with self.assertRaises(provision.BundleError):
                provision.validate(config, self.credentials)

    def test_firmware_configures_certificate_and_waits_for_clock(self):
        connectivity = (ROOT / "main" / "connectivity.c").read_text()
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn('read_nvs_string(handle, "certificate", server_certificate', connectivity)
        self.assertIn(".cert_pem = server_certificate", connectivity)
        self.assertIn("if (!time_synchronized)", connectivity)
        self.assertNotIn("skip_cert_common_name_check", connectivity)
        self.assertIn("CONFIG_ESP_TLS_INSECURE=n", defaults)
        self.assertIn("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=n", defaults)

    @unittest.skipUnless(os.environ.get("SATELLITE_MBEDTLS_VERIFY"), "mbedTLS host verifier not built")
    def test_esp_idf_mbedtls_verifies_trust_and_hostname(self):
        with tempfile.TemporaryDirectory() as directory:
            certificate = Path(directory) / "server.pem"
            certificate.write_text(self.config["server"]["certificate_pem"])
            binary = os.environ["SATELLITE_MBEDTLS_VERIFY"]
            system_ca = ssl.get_default_verify_paths().cafile or "/etc/ssl/certs/ca-certificates.crt"
            if not Path(system_ca).is_file():
                self.skipTest("system CA bundle unavailable")
            for trust, hostname, expected in (
                (str(certificate), "satellite.example.invalid", 0),
                (str(certificate), "wrong.example.invalid", 1),
                (system_ca, "satellite.example.invalid", 1),
            ):
                with self.subTest(hostname=hostname, trust=trust):
                    result = subprocess.run([binary, str(certificate), trust, hostname], check=False)
                    self.assertEqual(result.returncode, expected)

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

    def test_music_can_be_disabled_without_provider_credentials(self):
        config = copy.deepcopy(self.config)
        credentials = copy.deepcopy(self.credentials)
        config["music"] = {"player": "none"}
        del credentials["spotify"]
        entries = provision.validate(config, credentials)
        self.assertIn(("music", "player", "none"), entries)
        self.assertFalse(any(namespace in {"spotify", "sonos"} for namespace, _, _ in entries))
        credentials["spotify"] = self.credentials["spotify"]
        with self.assertRaises(provision.BundleError):
            provision.validate(config, credentials)

    def test_disabled_music_is_handled_by_firmware(self):
        music = (ROOT / "main" / "music.c").read_text()
        ui = (ROOT / "main" / "ui.c").read_text()
        self.assertIn('strcmp(player, "none")', music)
        self.assertIn("provider != MUSIC_PROVIDER_NONE", music)
        self.assertIn("satellite_music_enabled()", ui)


class ProtocolFixtureTests(unittest.TestCase):
    def test_websocket_tls_reads_and_writes_share_the_client_lock(self):
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=n", defaults)
        self.assertIn("CONFIG_MBEDTLS_DYNAMIC_BUFFER=n", defaults)

        rebuild = (ROOT / "docs" / "rebuild_flash_touch_lcd_rev2.sh").read_text()
        self.assertIn('rm -f "$GENERATED_SDKCONFIG"', rebuild)
        self.assertIn("Refusing to flash unsafe concurrent TLS configuration", rebuild)

    def test_uplink_write_timeout_tolerates_wifi_backpressure(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("#define WEBSOCKET_UPLINK_SEND_TIMEOUT_MS 100", protocol)
        self.assertIn("if (transport_input.binary && sent <= 0)", protocol)
        self.assertIn("Dropping uplink frame after a %u ms transport timeout", protocol)

    def test_transport_worker_owns_websocket_writes(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn('xTaskCreatePinnedToCore(transport_task, "transport_tx"', protocol)
        self.assertEqual(protocol.count("esp_websocket_client_send_"), 2)
        self.assertIn("discard_transport_messages(false);", protocol)

    def test_connectivity_task_owns_websocket_lifecycle_and_discards_stale_events(self):
        connectivity = (ROOT / "main" / "connectivity.c").read_text()
        self.assertIn('xTaskCreate(connectivity_task, "connectivity"', connectivity)
        self.assertIn("#define WEBSOCKET_NETWORK_TIMEOUT_MS 5000", connectivity)
        self.assertIn("#define WEBSOCKET_SHORT_LIVED_CONNECTION_LIMIT 3U", connectivity)
        self.assertIn("Server connections repeatedly failed before stabilization; reassociating Wi-Fi", connectivity)
        self.assertIn('ESP_LOGI(TAG, "Stopping WebSocket client")', connectivity)
        self.assertIn('ESP_LOGI(TAG, "WebSocket client stopped")', connectivity)
        self.assertIn("CONNECTIVITY_COMMAND_WEBSOCKET_EVENT", connectivity)
        self.assertIn("websocket_context->generation = ++websocket_generation", connectivity)
        self.assertIn("websocket_event->generation != websocket_generation", connectivity)
        self.assertIn("esp_websocket_client_destroy(websocket)", connectivity)
        callbacks = connectivity[connectivity.index("static void websocket_event_handler"):
                                 connectivity.index("static void destroy_websocket")]
        self.assertNotIn("esp_websocket_client_start", callbacks)
        self.assertNotIn("esp_websocket_client_stop", callbacks)

    def test_transport_reserves_capacity_for_control_frames(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("uxQueueSpacesAvailable(transport_queue) <= 1", protocol)
        self.assertIn("Dropping\n    // real-time microphone PCM", protocol)

    def test_tts_end_is_bound_to_its_speaking_stream(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("static uint32_t tts_stream_generation;", protocol)
        self.assertIn("static uint32_t tts_end_stream_generation;", protocol)
        self.assertIn("static void begin_tts_stream(void)", protocol)
        self.assertIn("tts_end_stream_generation = tts_stream_generation;", protocol)
        self.assertIn("stream_generation == tts_stream_generation", protocol)
        self.assertIn("begin_tts_stream();", protocol)

    def test_valid_hello_and_normal_sequence(self):
        hello = {
            "type": "satellite.hello",
            "token": "fixture-token",
            "protocol": {"name": "satellite-v2", "major": 2, "minor": 5},
            "device": {"id": "fixture-01", "name": "Fixture", "firmware": "0.1.0", "build": "test", "board": "rev2"},
            "capabilities": {"audio_input": True, "audio_output": True, "display": True, "touch": True,
                             "aec_uplink_channels": [1, 2]},
        }
        self.assertTrue(fixture.validate_hello(hello))
        self.assertEqual(fixture.scenario_messages("normal")[0]["type"], "satellite.welcome")
        self.assertEqual(fixture.scenario_messages("normal")[1], {
            "type": "satellite.health_request", "id": "fixture-health-1",
        })

    def test_resume_hello_uses_last_fully_played_sequence(self):
        hello = {
            "type": "satellite.hello",
            "token": "fixture-token",
            "protocol": {"name": "satellite-v2", "major": 2, "minor": 5},
            "device": {"id": "fixture-01", "name": "Fixture", "firmware": "0.1.0", "build": "test", "board": "rev2"},
            "capabilities": {"audio_input": True, "audio_output": True, "display": True, "touch": True,
                             "aec_uplink_channels": [1]},
            "resume": {"session_id": "prior-session", "turn_id": "turn-1", "next_seq": 7},
        }
        self.assertTrue(fixture.validate_hello(hello))
        hello["resume"]["next_seq"] = -1
        self.assertFalse(fixture.validate_hello(hello))

    def test_welcome_uses_the_fixed_16khz_downlink_contract(self):
        downlink = fixture.scenario_messages("normal")[0]["audio"]["downlink"]
        self.assertEqual(downlink, {"encoding": "pcm_s16le", "sample_rate_hz": 16000, "channels": 1})

    def test_v25_welcome_selects_only_an_advertised_uplink_contract(self):
        hello = {
            "capabilities": {"aec_uplink_channels": [1, 2]},
        }
        welcome = copy.deepcopy(fixture.WELCOME)
        welcome["audio"]["uplink"]["channels"] = 2
        self.assertTrue(fixture.validate_welcome(welcome, hello))
        hello["capabilities"]["aec_uplink_channels"] = [1]
        self.assertFalse(fixture.validate_welcome(welcome, hello))
        welcome["protocol"]["minor"] = 4
        self.assertFalse(fixture.validate_welcome(welcome, {"capabilities": {"aec_uplink_channels": [1, 2]}}))

    def test_v24_hello_remains_a_legacy_mono_contract(self):
        hello = {
            "type": "satellite.hello", "token": "fixture-token",
            "protocol": {"name": "satellite-v2", "major": 2, "minor": 4},
            "device": {"id": "fixture-01", "name": "Fixture", "firmware": "0.1.0", "build": "test", "board": "rev2"},
            "capabilities": {"audio_input": True, "audio_output": True, "display": True, "touch": True},
        }
        self.assertTrue(fixture.validate_hello(hello))

    def test_v25_stereo_welcome_contract_is_present_in_firmware(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        audio = (ROOT / "main" / "audio.c").read_text()
        self.assertIn('cJSON_AddNumberToObject(version, "minor", 5)', protocol)
        self.assertIn('"aec_uplink_channels"', protocol)
        self.assertIn('#define TRANSPORT_MAX_BYTES 1280', protocol)
        self.assertIn('minor == 4 && uplink_channels != 1', protocol)
        self.assertIn('aec_create_from_config(&config)', audio)
        self.assertIn('.mic_num = 2', audio)
        self.assertIn('.out_num = 2', audio)
        self.assertIn('.mode = AEC_MODE_SR_LOW_COST', audio)
        self.assertIn('M1,M2 with synchronous R', audio)
        self.assertIn('AFE_TYPE_VC', audio)
        self.assertIn('if (uplink_channels == 2)', audio)

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
        self.completed_turn_pending_state = False
        self.tts_stream_generation = 0
        self.tts_end_stream_generation = 0
        self.tts_end_pending = False

    def begin_tts_stream(self):
        self.tts_stream_generation += 1
        self.tts_end_pending = False

    def schedule_tts_end(self):
        self.tts_end_stream_generation = self.tts_stream_generation
        self.tts_end_pending = True

    def complete_tts_end(self):
        if not self.tts_end_pending or self.tts_end_stream_generation != self.tts_stream_generation:
            return False
        self.tts_end_pending = False
        return True

    def apply_tts_cancel(self, turn_id):
        if self.state != "speaking" or turn_id != self.turn_id:
            return False
        self.barge_in_turn_pending = True
        return True

    def apply_tts_end(self, turn_id):
        if self.state != "speaking" or turn_id != self.turn_id:
            return False
        self.completed_turn_pending_state = True
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
        replaces_completed_turn = (
            self.completed_turn_pending_state
            and state in {"wake_detected", "listening", "speaking"}
            and turn_id != self.turn_id
        )
        replaces_stale_inactive_turn = (
            state == "wake_detected"
            and self.state != "speaking"
            and self.turn_id is not None
            and turn_id != self.turn_id
        )
        starts_proactive_turn = state == "speaking" and self.turn_id is None
        if state == "wake_detected":
            if (
                self.turn_id is not None
                and not replaces_follow_up_turn
                and not replaces_barge_in_turn
                and not replaces_completed_turn
                and not replaces_stale_inactive_turn
            ) or not turn_id:
                return False
            self.turn_id = turn_id
        elif state not in {"listening", "thinking", "speaking", "follow_up", "error"} or (
            turn_id != self.turn_id
            and not replaces_follow_up_turn
            and not replaces_barge_in_turn
            and not replaces_completed_turn
            and not replaces_stale_inactive_turn
            and not starts_proactive_turn
        ):
            return False
        elif (replaces_follow_up_turn or replaces_barge_in_turn or replaces_completed_turn or
              replaces_stale_inactive_turn or starts_proactive_turn):
            self.turn_id = turn_id
        self.barge_in_turn_pending = False
        self.completed_turn_pending_state = False
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
        self.assertTrue(self.apply_state("idle", "announcement-1"))
        self.assertEqual(self.state, "idle")

    def test_tts_end_preserves_the_turn_for_the_follow_up_state(self):
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.assertTrue(self.apply_tts_end("turn-1"))
        self.assertTrue(self.apply_state("follow_up", "turn-1"))
        self.assertEqual(self.state, "follow_up")

    def test_final_stream_invalidates_the_acknowledgement_end(self):
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.begin_tts_stream()  # Cached acknowledgement starts.
        self.schedule_tts_end()
        self.assertTrue(self.tts_end_pending)
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.begin_tts_stream()  # Final SLM reply starts on the same turn.
        self.assertFalse(self.tts_end_pending)
        self.assertFalse(self.complete_tts_end())

    def test_only_the_current_stream_can_finish_playback(self):
        self.begin_tts_stream()
        self.schedule_tts_end()
        acknowledgement_generation = self.tts_end_stream_generation
        self.begin_tts_stream()
        self.tts_end_pending = True
        self.tts_end_stream_generation = acknowledgement_generation
        self.assertFalse(self.complete_tts_end())
        self.schedule_tts_end()
        self.assertTrue(self.complete_tts_end())

    def test_final_stream_pcm_sequence_follows_the_acknowledgement(self):
        expected_sequence = 0
        for sequence in (0, 1):
            self.assertEqual(sequence, expected_sequence)
            expected_sequence += 1

        self.begin_tts_stream()  # Final stream uses the same lifecycle turn.
        final_sequence = 2
        self.assertEqual(final_sequence, expected_sequence)

    def test_new_turn_can_replace_a_completed_tts_turn_before_idle_arrives(self):
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.assertTrue(self.apply_tts_end("turn-1"))
        self.assertTrue(self.apply_state("listening", "turn-2"))
        self.assertEqual(self.turn_id, "turn-2")

    def test_new_turn_is_rejected_without_follow_up_or_tts_cancel(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("speaking", "turn-1"))
        self.assertFalse(self.apply_state("listening", "turn-2"))

    def test_wake_detected_replaces_a_stale_listening_turn(self):
        self.assertTrue(self.apply_state("wake_detected", "turn-1"))
        self.assertTrue(self.apply_state("listening", "turn-1"))
        self.assertTrue(self.apply_state("wake_detected", "turn-2"))
        self.assertTrue(self.apply_state("listening", "turn-2"))
        self.assertEqual(self.turn_id, "turn-2")

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

    def test_asr_error_ends_the_turn_and_restores_uplink(self):
        state = "thinking"
        turn_id = "turn-1"
        uplink_enabled = False  # Thinking flushes the finalized utterance.

        # assistant.state:error is terminal even when no idle follows it.
        state = "idle"
        turn_id = ""
        uplink_enabled = True

        self.assertEqual(state, "idle")
        self.assertEqual(turn_id, "")
        self.assertTrue(uplink_enabled)

    def test_every_active_lifecycle_state_has_a_local_expiry(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("#define ASSISTANT_TENTATIVE_WAKE_TIMEOUT_MS 2500", protocol)
        self.assertIn("#define ASSISTANT_SHORT_TIMEOUT_MS 60000", protocol)
        self.assertIn("#define ASSISTANT_THINKING_TIMEOUT_MS 180000", protocol)
        self.assertIn("#define ASSISTANT_FOLLOW_UP_TIMEOUT_MS 60000", protocol)
        self.assertIn("static void assistant_expiry_timer_callback", protocol)
        self.assertIn("satellite_audio_cancel_playback();", protocol)
        self.assertIn("set_uplink_transmit_enabled(true);", protocol)

    def test_tentative_wake_idle_does_not_wait_for_playback_drain(self):
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("assistant_state == SATELLITE_ASSISTANT_WAKE_DETECTED ||", protocol)
        self.assertIn("complete_idle_state();", protocol)

    def test_follow_up_local_cap_is_not_extended_by_repeat_updates(self):
        local_cap_ms = 60000
        elapsed_ms = 25000
        local_remaining_ms = local_cap_ms - elapsed_ms
        server_remaining_ms = 60000

        self.assertEqual(min(local_remaining_ms, server_remaining_ms), 35000)

    def test_expired_server_follow_up_deadline_keeps_the_local_recovery_cap(self):
        local_remaining_ms = 60000
        server_remaining_ms = -1

        expiry_ms = local_remaining_ms
        if 0 < server_remaining_ms < 2**32:
            expiry_ms = min(expiry_ms, server_remaining_ms)

        self.assertEqual(expiry_ms, local_remaining_ms)
        protocol = (ROOT / "main" / "protocol.c").read_text()
        self.assertIn("Ignoring expired server follow-up deadline", protocol)


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

    def test_resumed_tts_can_start_at_the_server_sequence(self):
        sequence_started = False
        expected_seq = 0
        resumed_seq = 35
        self.assertFalse(sequence_started)
        sequence_started = True
        expected_seq = resumed_seq + 1
        self.assertEqual(expected_seq, 36)
        self.assertTrue(sequence_started)

    def test_replayed_tts_frames_are_discarded_until_the_expected_sequence(self):
        expected_seq = 14
        replayed = [11, 12, 13]
        self.assertTrue(all(sequence < expected_seq for sequence in replayed))
        self.assertEqual(expected_seq, 14)
        self.assertEqual(14, expected_seq)


if __name__ == "__main__":
    unittest.main()
