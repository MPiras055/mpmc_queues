"""Small reusable pieces of the sidebar.

Nothing here knows about plots; they are the vocabulary the window is written in, so that
:mod:`app` reads as a description of the interface rather than a pile of layout calls.
"""

from __future__ import annotations

from typing import Callable, Iterable

from PySide6 import QtCore, QtGui, QtWidgets

__all__ = ["Card", "heading", "hint", "Segmented", "Swatch", "Choice", "FilterGroup",
           "CommandPalette", "form_row"]


class Card(QtWidgets.QFrame):
    """A titled group of controls, flat with a hairline border."""

    def __init__(self, title: str = "", parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("Card")
        self._box = QtWidgets.QVBoxLayout(self)
        self._box.setContentsMargins(12, 10, 12, 12)
        self._box.setSpacing(7)
        if title:
            self._box.addWidget(heading(title))

    def add(self, widget: QtWidgets.QWidget) -> QtWidgets.QWidget:
        self._box.addWidget(widget)
        return widget

    def add_layout(self, layout: QtWidgets.QLayout) -> QtWidgets.QLayout:
        self._box.addLayout(layout)
        return layout

    @property
    def body(self) -> QtWidgets.QVBoxLayout:
        return self._box


def heading(text: str) -> QtWidgets.QLabel:
    label = QtWidgets.QLabel(text.upper())
    label.setObjectName("Heading")
    # Wrapping rather than widening: a long parameter name ("producer delay amplitude") set
    # the minimum width of the whole sidebar, and the scroll area has no horizontal bar.
    label.setWordWrap(True)
    return label


def hint(text: str) -> QtWidgets.QLabel:
    label = QtWidgets.QLabel(text)
    label.setObjectName("Hint")
    label.setWordWrap(True)
    return label


def form_row(label: str, widget: QtWidgets.QWidget) -> QtWidgets.QHBoxLayout:
    row = QtWidgets.QHBoxLayout()
    row.setSpacing(8)
    tag = QtWidgets.QLabel(label)
    tag.setMinimumWidth(92)
    row.addWidget(tag)
    row.addWidget(widget, 1)
    return row


class Segmented(QtWidgets.QWidget):
    """An exclusive row of buttons -- the modern replacement for a pair of radio buttons."""

    changed = QtCore.Signal(str)

    def __init__(self, options: dict[str, str], parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("Segmented")
        row = QtWidgets.QHBoxLayout(self)
        row.setContentsMargins(3, 3, 3, 3)
        row.setSpacing(3)
        self._group = QtWidgets.QButtonGroup(self)
        self._group.setExclusive(True)
        self._buttons: dict[str, QtWidgets.QToolButton] = {}
        for key, text in options.items():
            button = QtWidgets.QToolButton()
            button.setText(text)
            button.setCheckable(True)
            button.setCursor(QtCore.Qt.PointingHandCursor)
            self._group.addButton(button)
            row.addWidget(button)
            self._buttons[key] = button
            button.clicked.connect(lambda _=False, k=key: self.changed.emit(k))

    def set_value(self, key: str) -> None:
        button = self._buttons.get(key)
        if button is not None:
            button.setChecked(True)


class Swatch(QtWidgets.QToolButton):
    """A colour well that opens the colour dialog."""

    picked = QtCore.Signal(str)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("Swatch")
        self.setFixedSize(22, 22)
        self.setCursor(QtCore.Qt.PointingHandCursor)
        self._color = "#888888"
        self.clicked.connect(self._choose)

    def set_color(self, value: str) -> None:
        self._color = value
        self.setStyleSheet(f"background: {value}; border: 1px solid rgba(0,0,0,0.25);"
                           f"border-radius: 5px;")

    def _choose(self) -> None:
        chosen = QtWidgets.QColorDialog.getColor(QtGui.QColor(self._color), self,
                                                 "Series colour")
        if chosen.isValid():
            self.set_color(chosen.name())
            self.picked.emit(chosen.name())


class Choice(QtWidgets.QComboBox):
    """A combo box addressed by key rather than by the text on screen."""

    chosen = QtCore.Signal(str)

    def __init__(self, options: dict[str, str] | None = None, parent=None) -> None:
        super().__init__(parent)
        self._keys: list[str] = []
        self.set_options(options or {})
        self.currentIndexChanged.connect(self._emit)
        self._muted = False

    def set_options(self, options: dict[str, str]) -> None:
        previous = self.value
        self._muted = True
        self.clear()
        self._keys = list(options)
        self.addItems([options[k] for k in self._keys])
        if previous in self._keys:
            # Still muted: restoring what was already chosen is not a choice, and emitting
            # here fired a spurious edit on every reconcile.
            self.setCurrentIndex(self._keys.index(previous))
        self._muted = False

    @property
    def value(self) -> str:
        i = self.currentIndex()
        return self._keys[i] if 0 <= i < len(self._keys) else ""

    def set_value(self, key: str) -> None:
        if key in self._keys and key != self.value:
            self._muted = True
            self.setCurrentIndex(self._keys.index(key))
            self._muted = False

    def _emit(self, _index: int) -> None:
        if not self._muted:
            self.chosen.emit(self.value)


class FilterGroup(QtWidgets.QWidget):
    """One run parameter: its values, and what ticking more than one of them means.

    Three modes, because "tick a second queue size" means three different things:

    - **one** -- a radio button. Ticking a value unticks the previous one, which is what
      switching from 1024 to 4096 should do, and the last ticked box cannot be unticked
      (an empty filter plots nothing and reads as a bug rather than as a choice).
    - **facet** -- a plot per value, side by side. The default once compare is ticked.
    - **overlay** -- several lines on one plot. What a thread count needs, and what the
      column on the x axis is locked to.

    @signal changed (column, selected values)
    @signal modeChanged (column, mode)
    """

    changed = QtCore.Signal(str, list)
    modeChanged = QtCore.Signal(str, str)

    def __init__(self, column: str, title: str, values: Iterable[str],
                 selected: Iterable[str], linked: str = "", mode: str = "one",
                 locked: bool = False, parent=None) -> None:
        super().__init__(parent)
        self.column = column
        self._muted = True
        self._locked = locked
        box = QtWidgets.QVBoxLayout(self)
        box.setContentsMargins(0, 0, 0, 6)
        box.setSpacing(3)

        head = QtWidgets.QHBoxLayout()
        head.setSpacing(6)
        head.addWidget(heading(title), 1)
        self.compare = QtWidgets.QCheckBox("compare")
        self.compare.setChecked(locked or mode != "one")
        self.compare.setEnabled(not locked)
        self.compare.toggled.connect(self._compare_toggled)
        head.addWidget(self.compare, 0, QtCore.Qt.AlignTop)
        box.addLayout(head)

        # On its own row, and only while it applies: beside the heading it set the minimum
        # width of the sidebar all by itself.
        self.how = Choice({"facet": "separate plots", "overlay": "same plot"})
        self.how.set_value("overlay" if (locked or mode == "overlay") else "facet")
        self.how.setVisible(self.compare.isChecked() and not locked)
        self.how.chosen.connect(lambda _v: self._announce_mode())
        box.addWidget(self.how)

        if locked:
            box.addWidget(hint("on the x axis: every value is plotted"))
        elif linked:
            box.addWidget(hint(f"moves with {linked}"))

        grid = QtWidgets.QGridLayout()
        grid.setSpacing(4)
        chosen = {str(v) for v in selected}
        self._boxes: dict[str, QtWidgets.QCheckBox] = {}
        for i, value in enumerate(values):
            check = QtWidgets.QCheckBox(str(value))
            check.setChecked(str(value) in chosen)
            check.toggled.connect(lambda on, v=str(value): self._toggled(v, on))
            grid.addWidget(check, i // 3, i % 3)
            self._boxes[str(value)] = check
        box.addLayout(grid)
        self._muted = False

    # -- selection -----------------------------------------------------------------------

    @property
    def mode(self) -> str:
        if self._locked:
            return "overlay"
        return self.how.value if self.compare.isChecked() else "one"

    @property
    def selected(self) -> list[str]:
        return [v for v, b in self._boxes.items() if b.isChecked()]

    def set_selected(self, values: Iterable[str]) -> None:
        wanted = [str(v) for v in values]
        self._show(wanted if wanted else self.selected[:1])

    def set_mode(self, mode: str) -> None:
        self._muted = True
        self.compare.setChecked(mode != "one")
        if mode != "one":
            self.how.set_value(mode)
        self.how.setVisible(mode != "one" and not self._locked)
        self._muted = False

    def _show(self, values: list[str]) -> None:
        """Tick exactly @p values, without telling anyone: the caller is the one deciding."""
        wanted = set(values)
        for value, box in self._boxes.items():
            box.blockSignals(True)
            box.setChecked(value in wanted)
            box.blockSignals(False)

    def _toggled(self, value: str, on: bool) -> None:
        if self._muted:
            return
        if self.mode == "one" and on:
            self._show([value])                  # a radio button, not a tick box
        elif not self.selected:
            self._show([value])                  # refuse to leave the parameter with nothing
            return
        self.changed.emit(self.column, self.selected)

    # -- mode ----------------------------------------------------------------------------

    def _compare_toggled(self, on: bool) -> None:
        self.how.setVisible(on and not self._locked)
        if self._muted:
            return
        if not on and len(self.selected) > 1:
            self._show(self.selected[:1])        # back to one value, the first ticked
            self.changed.emit(self.column, self.selected)
        self._announce_mode()

    def _announce_mode(self) -> None:
        if not self._muted:
            self.modeChanged.emit(self.column, self.mode)


class CommandPalette(QtWidgets.QDialog):
    """Ctrl+K: every action in one searchable list.

    A sidebar with four tabs hides things. This is the escape hatch, and it doubles as the
    keyboard route to actions that have no button.
    """

    def __init__(self, actions: dict[str, Callable[[], None]], parent=None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Commands")
        self.setModal(True)
        self.resize(460, 380)
        self._actions = actions
        box = QtWidgets.QVBoxLayout(self)
        box.setContentsMargins(12, 12, 12, 12)
        box.setSpacing(8)
        self.search = QtWidgets.QLineEdit()
        self.search.setPlaceholderText("Type a command…")
        self.list = QtWidgets.QListWidget()
        box.addWidget(self.search)
        box.addWidget(self.list, 1)
        self.search.textChanged.connect(self._filter)
        self.search.returnPressed.connect(self._run_current)
        self.list.itemActivated.connect(lambda _item: self._run_current())
        self._filter("")
        self.search.setFocus()

    def _filter(self, text: str) -> None:
        needle = text.lower().strip()
        self.list.clear()
        for name in self._actions:
            if all(part in name.lower() for part in needle.split()):
                self.list.addItem(name)
        if self.list.count():
            self.list.setCurrentRow(0)

    def _run_current(self) -> None:
        item = self.list.currentItem()
        if item is None:
            return
        action = self._actions.get(item.text())
        self.accept()
        if action is not None:
            action()

    def keyPressEvent(self, event: QtGui.QKeyEvent) -> None:
        if event.key() in (QtCore.Qt.Key_Down, QtCore.Qt.Key_Up) and self.list.count():
            step = 1 if event.key() == QtCore.Qt.Key_Down else -1
            self.list.setCurrentRow((self.list.currentRow() + step) % self.list.count())
            event.accept()
            return
        super().keyPressEvent(event)
