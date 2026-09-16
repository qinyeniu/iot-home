import unittest

from app.services.topics import device_command_topic, split_device_id


class TopicHelperTests(unittest.TestCase):
    def test_current_multipart_device_id(self):
        self.assertEqual(split_device_id("gw-001-zb-2b6a"), ("gw-001", "zb-2b6a"))
        self.assertEqual(
            device_command_topic("iot-home", "gw-001-combined-01"),
            "iot-home/gw-001/nodes/combined-01/cmd",
        )

    def test_legacy_device_id(self):
        self.assertEqual(split_device_id("sensor-01"), ("gw-001", "sensor-01"))
        self.assertEqual(split_device_id("switch-01"), ("gw-001", "switch-01"))
        self.assertEqual(
            split_device_id("combined-01"), ("gw-001", "combined-01")
        )
        self.assertEqual(
            device_command_topic("iot-home", "sensor-01"),
            "iot-home/gw-001/nodes/sensor-01/cmd",
        )

    def test_invalid_device_id(self):
        for bad in ("", "gw001", "only-one-piece"):
            with self.subTest(bad=bad):
                self.assertIsNone(split_device_id(bad))
                self.assertIsNone(device_command_topic("iot-home", bad))


if __name__ == "__main__":
    unittest.main()
