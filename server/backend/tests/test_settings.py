"""应用配置测试：MQTT 分离账号与旧共享账号回退。"""

import unittest

from pydantic import ValidationError

from app.config import Settings


class SettingsTests(unittest.TestCase):
    def test_legacy_mqtt_account_is_used_when_backend_account_empty(self):
        settings = Settings(
            MQTT_USER="legacy-user",
            MQTT_PASSWORD="legacy-password",
            MQTT_BACKEND_USER="",
            MQTT_BACKEND_PASSWORD="",
        )

        self.assertEqual(settings.mqtt_username, "legacy-user")
        self.assertEqual(settings.mqtt_password, "legacy-password")

    def test_backend_account_takes_precedence(self):
        settings = Settings(
            MQTT_USER="legacy-user",
            MQTT_PASSWORD="legacy-password",
            MQTT_BACKEND_USER="backend-user",
            MQTT_BACKEND_PASSWORD="backend-password",
        )

        self.assertEqual(settings.mqtt_username, "backend-user")
        self.assertEqual(settings.mqtt_password, "backend-password")

    def test_default_gateway_id_matches_firmware(self):
        self.assertEqual(Settings().MQTT_GATEWAY_ID, "gw-001")

    def test_backend_user_without_password_is_rejected(self):
        with self.assertRaises(ValidationError):
            Settings(
                MQTT_USER="legacy-user",
                MQTT_PASSWORD="legacy-password",
                MQTT_BACKEND_USER="backend-user",
                MQTT_BACKEND_PASSWORD="",
            )

    def test_backend_password_without_user_is_rejected(self):
        with self.assertRaises(ValidationError):
            Settings(
                MQTT_USER="legacy-user",
                MQTT_PASSWORD="legacy-password",
                MQTT_BACKEND_USER="",
                MQTT_BACKEND_PASSWORD="backend-password",
            )


if __name__ == "__main__":
    unittest.main()
