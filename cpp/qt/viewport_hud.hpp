#pragma once

// A translucent, glanceable overlay shown over the viewport (top-left): the
// loaded file name and live alive/total counts, like the info card in Blender /
// 3D Slicer. Pure presentation — MainWindow feeds it via SetInfo(); it owns no
// app state and is transparent to mouse events so it never eats camera drags.

#include <QFrame>

class QLabel;

namespace tracto {

class ViewportHud : public QFrame {
  Q_OBJECT
 public:
  explicit ViewportHud(QWidget* parent = nullptr);

  void SetInfo(const QString& title, qulonglong alive, qulonglong total);
  void ShowEmpty();

 private:
  QLabel* titleLabel_;
  QLabel* countLabel_;
};

}  // namespace tracto
