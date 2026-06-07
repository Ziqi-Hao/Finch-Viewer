#include "viewport_hud.hpp"

#include <QFontDatabase>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

namespace tracto {

ViewportHud::ViewportHud(QWidget* parent) : QFrame(parent) {
  setObjectName("hud");  // styled by theme.qss (#hud)
  setAttribute(Qt::WA_TransparentForMouseEvents);  // never steal viewport drags

  titleLabel_ = new QLabel(this);
  titleLabel_->setObjectName("hudTitle");
  titleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);

  countLabel_ = new QLabel(this);
  countLabel_->setObjectName("hudCount");
  countLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  // Monospace so the live counts don't jitter the layout as digits change.
  countLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(2);
  layout->addWidget(titleLabel_);
  layout->addWidget(countLabel_);

  ShowEmpty();
}

void ViewportHud::SetInfo(const QString& title, qulonglong alive, qulonglong total) {
  const QLocale loc;
  titleLabel_->setText(title);
  countLabel_->setText(QStringLiteral("alive %1 / %2").arg(loc.toString(alive), loc.toString(total)));
  adjustSize();
  show();
}

void ViewportHud::ShowEmpty() {
  titleLabel_->setText(QStringLiteral("Finch-Viewer"));
  countLabel_->setText(QStringLiteral("no tractogram loaded"));
  adjustSize();
}

}  // namespace tracto
