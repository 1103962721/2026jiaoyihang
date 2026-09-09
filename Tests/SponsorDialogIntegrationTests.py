from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = [
    ROOT / "DF_PlayClick_Client_Qt",
    ROOT / "DF_PlayClick_Server_Qt",
]

REQUIRED_TEXT = [
    "一块不嫌少，两块不嫌多。",
    "本软件完全免费，如果你是买的那说明被人圈了",
]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")


def test_sponsor_dialog_is_integrated_in_qt_projects():
    for project in PROJECTS:
        assert (project / "SponsorWechat.png").is_file(), project
        assert (project / "SponsorDialog.h").is_file(), project
        assert (project / "SponsorDialog.cpp").is_file(), project

        dialog_text = read_text(project / "SponsorDialog.cpp")
        for text in REQUIRED_TEXT:
            assert text in dialog_text
        assert ":/SponsorWechat.png" in dialog_text

        assert "SponsorWechat.png" in read_text(project / "AppIcon.qrc")
        assert "SponsorDialog.cpp" in read_text(project / f"{project.name}.vcxproj")
        assert "SponsorDialog.h" in read_text(project / f"{project.name}.vcxproj")
        assert "SponsorDialog" in read_text(project / "main.cpp")
