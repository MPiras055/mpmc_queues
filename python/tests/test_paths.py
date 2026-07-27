"""Path resolution anchors to the package, never to the working directory."""

from __future__ import annotations

import pytest

from mpmc_bench import paths


def test_repo_root_is_found_from_the_package(monkeypatch, tmp_path):
    """The old scripts used '../../build', so cd-ing anywhere else broke them."""
    monkeypatch.chdir(tmp_path)
    root = paths.repo_root()
    assert (root / "CMakeLists.txt").is_file()
    assert (root / "include").is_dir()


def test_env_override_is_honoured(tmp_path, monkeypatch):
    (tmp_path / "CMakeCache.txt").write_text("")
    monkeypatch.setenv("MPMC_BUILD_DIR", str(tmp_path))
    assert paths.build_dir() == tmp_path.resolve()


def test_explicit_override_beats_env(tmp_path, monkeypatch):
    a, b = tmp_path / "a", tmp_path / "b"
    for d in (a, b):
        d.mkdir(); (d / "CMakeCache.txt").write_text("")
    monkeypatch.setenv("MPMC_BUILD_DIR", str(b))
    assert paths.build_dir(a) == a.resolve()


def test_directory_without_cmakecache_is_not_a_build_tree(tmp_path, monkeypatch):
    """A plausible-looking path must not pass, or the failure surfaces much later."""
    monkeypatch.setenv("MPMC_BUILD_DIR", str(tmp_path))
    monkeypatch.setattr(paths, "repo_root", lambda: tmp_path)
    with pytest.raises(paths.BuildNotFound) as exc:
        paths.build_dir()
    assert str(tmp_path) in str(exc.value)  # says where it looked
