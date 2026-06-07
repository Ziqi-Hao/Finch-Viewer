#include "theme.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>

namespace tracto {

QString StyleSheet() {
  // Authored with @token placeholders, then substituted from theme:: constants
  // so the hex values live in exactly one place (theme.hpp).
  QString qss = R"qss(
    * { outline: none; }
    QWidget { background-color: @bg1; color: @text; font-size: 12px; }
    QMainWindow { background-color: @bg0; }
    QMainWindow::separator { background: @hairline; width: 1px; height: 1px; }

    QMenuBar { background-color: @bg1; border-bottom: 1px solid @hairline; padding: 2px 4px; }
    QMenuBar::item { background: transparent; color: @textDim; padding: 4px 10px; border-radius: 4px; }
    QMenuBar::item:selected { background: @bg2; color: @text; }
    QMenuBar::item:pressed { background: @accent; color: @onAccent; }

    QMenu { background-color: @bg2; border: 1px solid @hairline; padding: 4px; }
    QMenu::item { color: @text; padding: 5px 24px 5px 12px; border-radius: 4px; }
    QMenu::item:selected { background: @accent; color: @onAccent; }
    QMenu::separator { height: 1px; background: @hairline; margin: 4px 8px; }

    QToolBar { background-color: @bg1; border: none; border-bottom: 1px solid @hairline;
               padding: 4px 6px; spacing: 2px; }
    QToolBar::separator { background: @hairline; width: 1px; margin: 4px 6px; }
    QToolButton { background: transparent; color: @textDim; font-weight: 500;
                  padding: 5px 10px; border-radius: 4px; }
    QToolButton:hover { background: @bg2; color: @text; }
    QToolButton:pressed { background: @accent; color: @onAccent; }
    QToolButton#dangerButton:hover { background: @danger; color: @onAccent; }

    QStatusBar { background-color: @bg1; border-top: 1px solid @hairline; color: @textDim; }
    QStatusBar::item { border: none; }

    QPushButton { background: @bg2; color: @text; border: 1px solid @hairline;
                  padding: 5px 14px; border-radius: 4px; }
    QPushButton:hover { border-color: @accent; }
    QPushButton:pressed { background: @accent; color: @onAccent; }
    QPushButton:default { border-color: @accent; }

    QToolTip { background-color: @bg2; color: @text; border: 1px solid @hairline; padding: 4px 6px; }

    QScrollBar:vertical { background: @bg1; width: 10px; margin: 0; }
    QScrollBar::handle:vertical { background: @hairline; min-height: 24px; border-radius: 5px; }
    QScrollBar::handle:vertical:hover { background: @textMuted; }
    QScrollBar:horizontal { background: @bg1; height: 10px; margin: 0; }
    QScrollBar::handle:horizontal { background: @hairline; min-width: 24px; border-radius: 5px; }
    QScrollBar::handle:horizontal:hover { background: @textMuted; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }

    /* Dock + cards (the right-hand Properties inspector). */
    QDockWidget { color: @textDim; titlebar-close-icon: none; titlebar-normal-icon: none; }
    /* Qt QSS has no text-transform; uppercase the title string at the source if wanted. */
    QDockWidget::title { background: @bg1; padding: 6px 10px; border-bottom: 1px solid @hairline; }
    QGroupBox { background: @bg2; border: 1px solid @hairline; border-radius: 6px;
                margin-top: 14px; padding: 8px 10px 10px 10px; }
    QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px;
                       padding: 0 4px; color: @textDim; font-weight: 600; }

    QLabel { background: transparent; }

    QSpinBox, QDoubleSpinBox { background: @bg1; color: @text; border: 1px solid @hairline;
               border-radius: 4px; padding: 2px 6px; min-width: 56px; }
    QSpinBox:focus, QDoubleSpinBox:focus { border-color: @accent; }
    QSpinBox::up-button, QSpinBox::down-button,
    QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 14px; background: @bg2;
               border-left: 1px solid @hairline; }
    QSpinBox::up-button:hover, QSpinBox::down-button:hover,
    QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @hairline; }
    QCheckBox { background: transparent; spacing: 6px; }
    QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @hairline;
               border-radius: 3px; background: @bg1; }
    QCheckBox::indicator:checked { background: @accent; border-color: @accent; }

    QSlider::groove:horizontal { height: 4px; background: @bg1; border-radius: 2px; }
    QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
    QSlider::handle:horizontal { background: @text; width: 12px; height: 12px;
               margin: -5px 0; border-radius: 6px; }
    QSlider::handle:horizontal:hover { background: @accent; }

    /* Translucent overlay floating over the viewport (Blender/Slicer-style). */
    QFrame#hud { background-color: rgba(18, 21, 27, 0.72); border: 1px solid @hairline;
                 border-radius: 8px; }
    QLabel#hudTitle { background: transparent; color: @text; font-size: 13px; font-weight: 600; }
    QLabel#hudCount { background: transparent; color: @textDim; font-size: 12px; }
  )qss";

  qss.replace("@bg0", theme::kBg0)
      .replace("@bg1", theme::kBg1)
      .replace("@bg2", theme::kBg2)
      .replace("@hairline", theme::kHairline)
      .replace("@textDim", theme::kTextDim)
      .replace("@textMuted", theme::kTextMuted)
      .replace("@text", theme::kText)
      .replace("@accent", theme::kAccent)
      .replace("@onAccent", theme::kOnAccent)
      .replace("@danger", theme::kDanger);
  return qss;
}

void ApplyTheme(QApplication& app) {
  app.setStyle("Fusion");  // a QSS-friendly base that looks consistent cross-platform

  // A dark palette so widgets QSS doesn't reach (file dialogs, message boxes)
  // still follow the theme.
  QPalette p;
  const QColor bg0(theme::kBg0), bg1(theme::kBg1), bg2(theme::kBg2);
  const QColor text(theme::kText), muted(theme::kTextMuted), accent(theme::kAccent);
  p.setColor(QPalette::Window, bg1);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, bg0);
  p.setColor(QPalette::AlternateBase, bg1);
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, bg2);
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::ToolTipBase, bg2);
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::Highlight, accent);
  p.setColor(QPalette::HighlightedText, QColor(theme::kOnAccent));
  p.setColor(QPalette::PlaceholderText, muted);
  p.setColor(QPalette::Disabled, QPalette::Text, muted);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
  app.setPalette(p);

  app.setStyleSheet(StyleSheet());
}

}  // namespace tracto
