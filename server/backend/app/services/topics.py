"""Helpers for MQTT topic and device identifier conventions."""

from typing import Optional, Tuple


def split_device_id(device_id: str) -> Optional[Tuple[str, str]]:
    """Return ``(gateway_id, node_id)`` for a persisted device identifier.

    Current identifiers look like ``gw-001-zb-2b6a``. The legacy
    ``sensor-01`` form is retained for older local demos and maps to the
    single default gateway ``gw-001``.
    """
    if not isinstance(device_id, str) or not device_id:
        return None

    parts = device_id.split("-", 2)
    if len(parts) == 3 and parts[0] == "gw" and parts[1].isdigit() and parts[2]:
        gateway_id = f"{parts[0]}-{parts[1]}"
        node_id = parts[2]
        return gateway_id, node_id

    legacy = device_id.split("-", 1)
    if len(legacy) == 2 and legacy[0] in {"sensor", "switch", "combined"}:
        return "gw-001", device_id

    return None


def device_command_topic(topic_prefix: str, device_id: str) -> Optional[str]:
    """Build a gateway command topic, or return ``None`` for a bad ID."""
    target = split_device_id(device_id)
    if target is None:
        return None
    gateway_id, node_id = target
    return f"{topic_prefix}/{gateway_id}/nodes/{node_id}/cmd"
