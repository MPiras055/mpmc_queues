"""The window's own colours, derived from the chart's.

The app and the figure inside it have to read as one thing, so the surface, text and grid
roles come straight from :mod:`plotting.theme` -- the same values the exported PNG uses --
and only the interface-specific tokens (accent, elevated panels, borders, radii) are added
here. Changing a chart colour therefore cannot leave the sidebar behind.

Everything is flat: no bevels, no gradients, no engraved frames. The old Tk window looked
dated mostly because ttk draws sunken borders around everything, and the fix is to stop
drawing them rather than to re-tint them.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..plotting import theme as charts

#: Qt style sheets cannot draw a tick, and setting a background on an indicator throws the
#: native one away -- so the mark is a file, referenced by absolute path.
_ICONS = Path(__file__).with_name("icons")

__all__ = ["Skin", "SKINS", "skin", "stylesheet"]


@dataclass(frozen=True)
class Skin:
    """Interface colours. The chart roles are reached through ``chart``."""

    name: str
    chart: charts.Theme
    window: str          # the app background, behind everything
    panel: str           # sidebars, docks, cards
    raised: str          # inputs and buttons sitting on a panel
    hover: str
    border: str
    accent: str          # the one saturated colour the interface is allowed
    accent_text: str
    text: str
    subtle: str
    ok: str
    warn: str
    danger: str

    @property
    def radius(self) -> int:
        return 8


SKINS = {
    "dark": Skin(
        name="dark", chart=charts.DARK,
        window="#121211", panel="#1a1a19", raised="#232322", hover="#2c2c2a",
        border="#333331", accent="#3987e5", accent_text="#ffffff",
        text="#ececea", subtle="#9a9892",
        ok="#199e70", warn="#c98500", danger="#e66767",
    ),
    "light": Skin(
        name="light", chart=charts.LIGHT,
        window="#f4f3ef", panel="#fcfcfb", raised="#ffffff", hover="#efeee8",
        border="#dedcd3", accent="#2a78d6", accent_text="#ffffff",
        text="#161615", subtle="#6f6e69",
        ok="#008300", warn="#8a5b00", danger="#c0392b",
    ),
}


def skin(name: str) -> Skin:
    return SKINS.get(name, SKINS["dark"])


def stylesheet(s: Skin, scale: float = 1.0) -> str:
    """Qt style sheet for @p s.

    @param scale multiplies every fixed pixel size, so the window stays usable on a HiDPI
                 screen without Qt's own device-pixel scaling being involved.
    """
    def px(n: float) -> str:
        return f"{max(1, round(n * scale))}px"

    r = px(s.radius)
    return f"""
    * {{ outline: 0; }}

    QWidget {{
        background: {s.window};
        color: {s.text};
        font-size: {px(13)};
    }}
    QMainWindow::separator {{ background: {s.border}; width: {px(1)}; height: {px(1)}; }}

    /* ---- surfaces -------------------------------------------------------------- */
    QFrame#Card, QScrollArea, QTabWidget::pane, QDockWidget > QWidget {{
        background: {s.panel};
        border: {px(1)} solid {s.border};
        border-radius: {r};
    }}
    QScrollArea {{ border: 0; }}
    QWidget#Sidebar, QWidget#Sidebar > QWidget {{ background: {s.panel}; }}
    QWidget#ChartHost {{ background: {s.chart.surface}; border-radius: {r}; }}

    QLabel#Heading {{
        color: {s.subtle}; font-weight: 600;
        font-size: {px(11)}; letter-spacing: {px(1)};
        padding: {px(10)} {px(2)} {px(4)} {px(2)};
    }}
    QLabel#Hint {{ color: {s.subtle}; font-size: {px(11)}; }}
    QLabel#Chip {{
        color: {s.subtle}; background: {s.raised};
        border: {px(1)} solid {s.border}; border-radius: {px(11)};
        padding: {px(3)} {px(10)};
    }}

    /* ---- controls -------------------------------------------------------------- */
    QPushButton, QToolButton {{
        background: {s.raised};
        border: {px(1)} solid {s.border};
        border-radius: {px(6)};
        padding: {px(6)} {px(12)};
        color: {s.text};
    }}
    QPushButton:hover, QToolButton:hover {{ background: {s.hover}; }}
    QPushButton:pressed, QToolButton:pressed {{ background: {s.border}; }}
    QPushButton:disabled, QToolButton:disabled {{ color: {s.subtle}; background: {s.panel}; }}
    QPushButton#Primary {{
        background: {s.accent}; color: {s.accent_text}; border: 0; font-weight: 600;
    }}
    QPushButton#Primary:hover {{ background: {s.accent}; }}
    QPushButton#Primary:disabled {{ background: {s.raised}; color: {s.subtle}; }}
    QToolButton#Swatch {{ border-radius: {px(5)}; padding: 0; }}

    QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {{
        background: {s.raised};
        border: {px(1)} solid {s.border};
        border-radius: {px(6)};
        padding: {px(5)} {px(8)};
        selection-background-color: {s.accent};
        selection-color: {s.accent_text};
    }}
    QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {{
        border-color: {s.accent};
    }}
    QComboBox::drop-down {{ border: 0; width: {px(18)}; }}
    QComboBox QAbstractItemView {{
        background: {s.raised}; border: {px(1)} solid {s.border};
        border-radius: {px(6)}; padding: {px(4)};
        selection-background-color: {s.accent}; selection-color: {s.accent_text};
    }}

    QCheckBox, QRadioButton {{ spacing: {px(8)}; padding: {px(2)} 0; }}
    QCheckBox::indicator, QRadioButton::indicator {{
        width: {px(15)}; height: {px(15)};
        border: {px(1)} solid {s.border}; border-radius: {px(4)};
        background: {s.raised};
    }}
    QRadioButton::indicator {{ border-radius: {px(8)}; }}
    QCheckBox::indicator:checked {{
        background: {s.accent}; border-color: {s.accent};
        image: url({_ICONS / 'check.svg'});
    }}
    QRadioButton::indicator:checked {{
        background: {s.accent}; border-color: {s.accent};
        image: url({_ICONS / 'dot.svg'});
    }}
    QCheckBox::indicator:hover, QRadioButton::indicator:hover {{ border-color: {s.accent}; }}

    /* ---- segmented control (the theme / mode switches) -------------------------- */
    QWidget#Segmented {{ background: {s.raised}; border-radius: {px(7)}; }}
    QWidget#Segmented QToolButton {{
        background: transparent; border: 0; border-radius: {px(6)};
        padding: {px(5)} {px(12)}; color: {s.subtle};
    }}
    QWidget#Segmented QToolButton:checked {{
        background: {s.panel}; color: {s.text}; font-weight: 600;
        border: {px(1)} solid {s.border};
    }}

    /* ---- lists, trees, tables --------------------------------------------------- */
    QTreeView, QTableView, QListView {{
        background: {s.panel}; border: {px(1)} solid {s.border}; border-radius: {r};
        alternate-background-color: {s.window};
        gridline-color: {s.border};
        selection-background-color: {s.accent}; selection-color: {s.accent_text};
    }}
    QTreeView::item, QTableView::item, QListView::item {{ padding: {px(4)} {px(6)}; }}
    QTreeView::item:hover, QTableView::item:hover {{ background: {s.hover}; }}
    QTreeView::indicator {{
        width: {px(15)}; height: {px(15)};
        border: {px(1)} solid {s.border}; border-radius: {px(4)};
        background: {s.raised};
    }}
    QTreeView::indicator:checked {{
        background: {s.accent}; border-color: {s.accent};
        image: url({_ICONS / 'check.svg'});
    }}
    QHeaderView::section {{
        background: {s.panel}; color: {s.subtle};
        border: 0; border-bottom: {px(1)} solid {s.border};
        padding: {px(6)}; font-weight: 600;
    }}

    /* ---- tabs ------------------------------------------------------------------- */
    QTabWidget::pane {{ top: {px(-1)}; }}
    QTabBar::tab {{
        background: transparent; color: {s.subtle};
        padding: {px(7)} {px(14)}; margin-right: {px(2)};
        border-bottom: {px(2)} solid transparent;
    }}
    QTabBar::tab:selected {{ color: {s.text}; border-bottom-color: {s.accent}; }}
    QTabBar::tab:hover {{ color: {s.text}; }}

    /* ---- scrollbars ------------------------------------------------------------- */
    QScrollBar:vertical, QScrollBar:horizontal {{
        background: transparent; margin: 0;
        width: {px(10)}; height: {px(10)};
    }}
    QScrollBar::handle {{ background: {s.border}; border-radius: {px(5)}; min-height: {px(28)}; }}
    QScrollBar::handle:hover {{ background: {s.subtle}; }}
    QScrollBar::add-line, QScrollBar::sub-line,
    QScrollBar::add-page, QScrollBar::sub-page {{ background: none; border: 0; height: 0; width: 0; }}

    /* ---- chrome ------------------------------------------------------------------ */
    QToolBar {{
        background: {s.window}; border: 0;
        padding: {px(6)} {px(8)}; spacing: {px(6)};
    }}
    QToolBar::separator {{
        background: {s.border}; width: {px(1)}; margin: {px(4)} {px(8)};
    }}
    QStatusBar {{ background: {s.window}; color: {s.subtle}; border: 0; }}
    QStatusBar::item {{ border: 0; }}

    /* ---- misc ------------------------------------------------------------------- */
    QToolTip {{
        background: {s.raised}; color: {s.text};
        border: {px(1)} solid {s.border}; border-radius: {px(6)}; padding: {px(6)};
    }}
    QSplitter::handle {{ background: transparent; }}
    QSplitter::handle:hover {{ background: {s.border}; }}
    QProgressBar {{
        background: {s.raised}; border: 0; border-radius: {px(3)};
        height: {px(6)}; text-align: center; color: transparent;
    }}
    QProgressBar::chunk {{ background: {s.accent}; border-radius: {px(3)}; }}
    QMenu {{
        background: {s.raised}; border: {px(1)} solid {s.border};
        border-radius: {px(8)}; padding: {px(5)};
    }}
    QMenu::item {{ padding: {px(6)} {px(22)}; border-radius: {px(5)}; }}
    QMenu::item:selected {{ background: {s.accent}; color: {s.accent_text}; }}
    QMenu::separator {{ height: {px(1)}; background: {s.border}; margin: {px(4)} {px(8)}; }}
    """
