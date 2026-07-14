#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""A place to keep secure channel keys while developing and testing."""

import os
import secrets
import tempfile

__all__ = ["KeyStore"]

KEY_LEN = 16
"""Length of a secure channel base key, in bytes."""


class KeyStore:
    """Generates secure channel keys and remembers them across runs.

    Keys live in memory until `commit_key()` writes them out. Without a
    directory they go to a temporary one that is removed when this object is,
    which is what tests want.

    Example:
        >>> store = KeyStore()
        >>> key = store.new_key("my-pd")
        >>> len(key)
        16
        >>> store.get_key("my-pd") == key
        True
    """

    def __init__(self, dir: str | None = None) -> None:
        self.temp_dir: tempfile.TemporaryDirectory[str] | None = None
        self.keys: dict[str, bytes] = {}
        if dir:
            self.key_dir = dir
        else:
            self.temp_dir = tempfile.TemporaryDirectory()
            self.key_dir = self.temp_dir.name

    def key_file(self, name: str) -> str:
        """Where the key called `name` is stored."""
        return os.path.join(self.key_dir, "key_" + name + ".bin")

    @staticmethod
    def gen_key(key_len: int = KEY_LEN) -> bytes:
        """Generate a new random key."""
        return secrets.token_bytes(key_len)

    def get_key(self, name: str) -> bytes:
        """Return a key held in memory. Raises KeyError if there is none."""
        if name not in self.keys:
            raise KeyError(f"No key named {name!r}")
        return self.keys[name]

    def new_key(
        self, name: str, key_len: int = KEY_LEN, force: bool = True
    ) -> bytes:
        """Generate a key and hold it under `name`.

        Raises KeyError if a key of that name exists and `force` is False.
        """
        if not force and name in self.keys:
            raise KeyError(f"Key {name!r} already exists")
        self.keys[name] = self.gen_key(key_len)
        return self.keys[name]

    def update_key(self, name: str, key: bytes) -> None:
        """Replace the key held under `name`."""
        if name not in self.keys:
            raise KeyError(f"No key named {name!r}")
        self.keys[name] = key

    def commit_key(self, name: str) -> None:
        """Write the key held under `name` to disk."""
        if name not in self.keys:
            raise KeyError(f"No key named {name!r}")
        with open(self.key_file(name), "w") as f:
            f.write(self.keys[name].hex())

    def load_key(self, name: str, key_len: int = KEY_LEN) -> bytes:
        """Read a previously committed key back from disk."""
        path = self.key_file(name)
        if not os.path.exists(path):
            raise FileNotFoundError(f"No stored key named {name!r}")
        with open(path, "r") as f:
            key = bytes.fromhex(f.read())
        if len(key) != key_len:
            raise ValueError(
                f"Stored key {name!r} is {len(key)} bytes, expected {key_len}"
            )
        self.keys[name] = key
        return key

    def __del__(self) -> None:
        if self.temp_dir:
            self.temp_dir.cleanup()
