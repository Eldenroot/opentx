/*
 * Copyright (C) OpenTX
 *
 * Based on code named
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "setup.h"
#include "ui_setup.h"
#include "ui_setup_timer.h"
#include "ui_setup_module.h"
#include "rawitemfilteredmodel.h"
#include "appdata.h"
#include "modelprinter.h"
#include "multiprotocols.h"
#include "checklistdialog.h"
#include "helpers.h"

#include <QDir>

TimerPanel::TimerPanel(QWidget *parent, ModelData & model, TimerData & timer, GeneralSettings & generalSettings, Firmware * firmware, QWidget * prevFocus, RawSwitchFilterItemModel * switchModel):
  ModelPanel(parent, model, generalSettings, firmware),
  timer(timer),
  ui(new Ui::Timer)
{
  Board::Type board = firmware->getBoard();

  ui->setupUi(this);

  lock = true;

  // Name
  int length = firmware->getCapability(TimersName);
  if (length == 0) {
    ui->name->hide();
  }
  else {
    ui->name->setMaxLength(length);
    ui->name->setText(timer.name);
  }

  // Mode
  ui->mode->setModel(switchModel);
  ui->mode->setCurrentIndex(ui->mode->findData(timer.mode.toValue()));
  connect(ui->mode, SIGNAL(activated(int)), this, SLOT(onModeChanged(int)));

  if (!firmware->getCapability(PermTimers)) {
    ui->persistent->hide();
    ui->persistentValue->hide();
  }

  ui->countdownBeep->setField(timer.countdownBeep, this);
  ui->countdownBeep->addItem(tr("Silent"), TimerData::COUNTDOWN_SILENT);
  ui->countdownBeep->addItem(tr("Beeps"), TimerData::COUNTDOWN_BEEPS);
  if (IS_ARM(board) || IS_2560(board)) {
    ui->countdownBeep->addItem(tr("Voice"), TimerData::COUNTDOWN_VOICE);
    ui->countdownBeep->addItem(tr("Haptic"), TimerData::COUNTDOWN_HAPTIC);
  }

  ui->value->setMaximumTime(firmware->getMaxTimerStart());

  ui->persistent->setField(timer.persistent, this);
  ui->persistent->addItem(tr("Not persistent"), 0);
  ui->persistent->addItem(tr("Persistent (flight)"), 1);
  ui->persistent->addItem(tr("Persistent (manual reset)"), 2);

  disableMouseScrolling();
  QWidget::setTabOrder(prevFocus, ui->name);
  QWidget::setTabOrder(ui->name, ui->value);
  QWidget::setTabOrder(ui->value, ui->mode);
  QWidget::setTabOrder(ui->mode, ui->countdownBeep);
  QWidget::setTabOrder(ui->countdownBeep, ui->minuteBeep);
  QWidget::setTabOrder(ui->minuteBeep, ui->persistent);

  lock = false;
}

TimerPanel::~TimerPanel()
{
  delete ui;
}

void TimerPanel::update()
{
  int hour = timer.val / 3600;
  int min = (timer.val - (hour * 3600)) / 60;
  int sec = (timer.val - (hour * 3600)) % 60;

  ui->mode->setCurrentIndex(ui->mode->findData(timer.mode.toValue()));
  ui->value->setTime(QTime(hour, min, sec));

  if (firmware->getCapability(PermTimers)) {
    int sign = 1;
    int pvalue = timer.pvalue;
    if (pvalue < 0) {
      pvalue = -pvalue;
      sign = -1;
    }
    int hours = pvalue / 3600;
    pvalue -= hours * 3600;
    int minutes = pvalue / 60;
    int seconds = pvalue % 60;
    ui->persistentValue->setText(QString(" %1(%2:%3:%4)").arg(sign<0 ? "-" :" ").arg(hours, 2, 10, QLatin1Char('0')).arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0')));
  }

  ui->minuteBeep->setChecked(timer.minuteBeep);
}

QWidget * TimerPanel::getLastFocus()
{
  return ui->persistent;
}

void TimerPanel::on_value_editingFinished()
{
  unsigned val = ui->value->time().hour()*3600 + ui->value->time().minute()*60 + ui->value->time().second();
  if (timer.val != val) {
    timer.val = val;
    emit modified();
  }
}

void TimerPanel::onModeChanged(int index)
{
  if (lock)
    return;

  bool ok;
  const RawSwitch rs(ui->mode->itemData(index).toInt(&ok));
  if (ok && timer.mode.toValue() != rs.toValue()) {
    timer.mode = rs;
    emit modified();
  }
}

void TimerPanel::on_minuteBeep_toggled(bool checked)
{
  timer.minuteBeep = checked;
  emit modified();
}

void TimerPanel::on_name_editingFinished()
{
  if (QString(timer.name) != ui->name->text()) {
    int length = ui->name->maxLength();
    strncpy(timer.name, ui->name->text().toLatin1(), length);
    emit modified();
  }
}

/******************************************************************************/

#define FAILSAFE_CHANNEL_HOLD    2000
#define FAILSAFE_CHANNEL_NOPULSE 2001

#define MASK_PROTOCOL       (1<<0)
#define MASK_CHANNELS_COUNT (1<<1)
#define MASK_RX_NUMBER      (1<<2)
#define MASK_CHANNELS_RANGE (1<<3)
#define MASK_PPM_FIELDS     (1<<4)
#define MASK_FAILSAFES      (1<<5)
#define MASK_OPEN_DRAIN     (1<<6)
#define MASK_MULTIMODULE    (1<<7)
#define MASK_ANTENNA        (1<<8)
#define MASK_MULTIOPTION    (1<<9)
#define MASK_R9M            (1<<10)
#define MASK_SBUSPPM_FIELDS (1<<11)
#define MASK_SUBTYPES       (1<<12)
#define MASK_ACCESS         (1<<13)

quint8 ModulePanel::failsafesValueDisplayType = ModulePanel::FAILSAFE_DISPLAY_PERCENT;

ModulePanel::ModulePanel(QWidget * parent, ModelData & model, ModuleData & module, GeneralSettings & generalSettings, Firmware * firmware, int moduleIdx):
  ModelPanel(parent, model, generalSettings, firmware),
  module(module),
  moduleIdx(moduleIdx),
  ui(new Ui::Module)
{
  lock = true;

  ui->setupUi(this);

  ui->label_module->setText(ModuleData::indexToString(moduleIdx, firmware));
  if (moduleIdx < 0) {
    if (IS_HORUS(firmware->getBoard())) {
      ui->trainerMode->setItemData(TRAINER_MODE_MASTER_CPPM_EXTERNAL_MODULE, 0, Qt::UserRole - 1);
      ui->trainerMode->setItemData(TRAINER_MODE_MASTER_SBUS_EXTERNAL_MODULE, 0, Qt::UserRole - 1);
    }
    if (generalSettings.auxSerialMode != UART_MODE_SBUS_TRAINER) {
      ui->trainerMode->setItemData(TRAINER_MODE_MASTER_BATTERY_COMPARTMENT, 0, Qt::UserRole - 1);
    }
    ui->trainerMode->setCurrentIndex(model.trainerMode);
    if (!IS_HORUS_OR_TARANIS(firmware->getBoard())) {
      ui->label_trainerMode->hide();
      ui->trainerMode->hide();
    }
    ui->formLayout_col1->setSpacing(0);
  }
  else {
    ui->label_trainerMode->hide();
    ui->trainerMode->hide();
  }

  // The protocols available on this board
  for (unsigned int i=0; i<PULSES_PROTOCOL_LAST; i++) {
    if (firmware->isAvailable((PulsesProtocol) i, moduleIdx)) {
      ui->protocol->addItem(ModuleData::protocolToString(i), i);
      if (i == module.protocol)
        ui->protocol->setCurrentIndex(ui->protocol->count()-1);
    }
  }
  for (int i=0; i<=MODULE_SUBTYPE_MULTI_LAST; i++) {
    ui->multiProtocol->addItem(Multiprotocols::protocolToString(i), i);
  }

  ui->btnGrpValueType->setId(ui->optPercent, FAILSAFE_DISPLAY_PERCENT);
  ui->btnGrpValueType->setId(ui->optUs, FAILSAFE_DISPLAY_USEC);
  ui->btnGrpValueType->button(failsafesValueDisplayType)->setChecked(true);

  ui->registrationId->setText(model.registrationId);

  setupFailsafes();

  disableMouseScrolling();

  update();

  connect(ui->protocol, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ModulePanel::onProtocolChanged);
  connect(ui->multiSubType, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ModulePanel::onSubTypeChanged);
  connect(ui->multiProtocol, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ModulePanel::onMultiProtocolChanged);
  connect(this, &ModulePanel::channelsRangeChanged, this, &ModulePanel::setupFailsafes);
  connect(ui->btnGrpValueType, static_cast<void(QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked), this, &ModulePanel::onFailsafesDisplayValueTypeChanged);

  connect(ui->clearRx1, SIGNAL(clicked()), this, SLOT(onClearAccessRxClicked()));
  connect(ui->clearRx2, SIGNAL(clicked()), this, SLOT(onClearAccessRxClicked()));
  connect(ui->clearRx3, SIGNAL(clicked()), this, SLOT(onClearAccessRxClicked()));

  lock = false;

}

ModulePanel::~ModulePanel()
{
  delete ui;
}

bool ModulePanel::moduleHasFailsafes()
{
  return firmware->getCapability(HasFailsafe) && (
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_ACCESS_ISRM ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_ACCST_ISRM_D16 ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_PXX_XJT_X16 ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_PXX_R9M ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_ACCESS_R9M ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_ACCESS_R9M_LITE ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_ACCESS_R9M_LITE_PRO ||
    (PulsesProtocol)module.protocol == PulsesProtocol::PULSES_XJT_LITE_X16);
}

void ModulePanel::setupFailsafes()
{
  ChannelFailsafeWidgetsGroup grp;
  const int start = module.channelsStart;
  const int end = start + module.channelsCount;
  const bool hasFailsafe = moduleHasFailsafes();

  lock = true;

  QMutableMapIterator<int, ChannelFailsafeWidgetsGroup> i(failsafeGroupsMap);
  while (i.hasNext()) {
    i.next();
    grp = i.value();
    ui->failsafesLayout->removeWidget(grp.label);
    ui->failsafesLayout->removeWidget(grp.combo);
    ui->failsafesLayout->removeWidget(grp.sbPercent);
    ui->failsafesLayout->removeWidget(grp.sbUsec);
    if (i.key() < start || i.key() >= end || !hasFailsafe) {
      grp.label->deleteLater();
      grp.combo->deleteLater();
      grp.sbPercent->deleteLater();
      grp.sbUsec->deleteLater();
      i.remove();
    }
  }

  if (!hasFailsafe)
    return;

  int row = 0;
  int col = 0;
  int channelMax = model->getChannelsMax();
  int channelMaxUs = 512 * channelMax / 100 * 2;

  for (int i = start; i < end; ++i) {
    if (failsafeGroupsMap.contains(i)) {
      grp = failsafeGroupsMap.value(i);
    }
    else {
      QLabel * label = new QLabel(this);
      label->setProperty("index", i);
      label->setText(QString::number(i+1));

      QComboBox * combo = new QComboBox(this);
      combo->setProperty("index", i);
      combo->addItem(tr("Value"), 0);
      combo->addItem(tr("Hold"), FAILSAFE_CHANNEL_HOLD);
      combo->addItem(tr("No Pulse"), FAILSAFE_CHANNEL_NOPULSE);

      QDoubleSpinBox * sbDbl = new QDoubleSpinBox(this);
      sbDbl->setProperty("index", i);
      sbDbl->setMinimumSize(QSize(20, 0));
      sbDbl->setRange(-channelMax, channelMax);
      sbDbl->setSingleStep(0.1);
      sbDbl->setDecimals(1);

      QSpinBox * sbInt = new QSpinBox(this);
      sbInt->setProperty("index", i);
      sbInt->setMinimumSize(QSize(20, 0));
      sbInt->setRange(-channelMaxUs, channelMaxUs);
      sbInt->setSingleStep(1);

      grp = ChannelFailsafeWidgetsGroup();
      grp.combo = combo;
      grp.sbPercent = sbDbl;
      grp.sbUsec = sbInt;
      grp.label = label;
      failsafeGroupsMap.insert(i, grp);

      connect(combo, SIGNAL(currentIndexChanged(int)), this, SLOT(onFailsafeComboIndexChanged(int)));
      connect(sbInt, SIGNAL(valueChanged(int)), this, SLOT(onFailsafeUsecChanged(int)));
      connect(sbDbl, SIGNAL(valueChanged(double)), this, SLOT(onFailsafePercentChanged(double)));
    }

    ui->failsafesLayout->addWidget(grp.label, row, col, Qt::AlignHCenter);
    ui->failsafesLayout->addWidget(grp.combo, row + 1, col, Qt::AlignHCenter);
    ui->failsafesLayout->addWidget(grp.sbPercent, row + 2, col, Qt::AlignHCenter);
    ui->failsafesLayout->addWidget(grp.sbUsec, row + 3, col, Qt::AlignHCenter);
    grp.sbPercent->setVisible(failsafesValueDisplayType == FAILSAFE_DISPLAY_PERCENT);
    grp.sbUsec->setVisible(failsafesValueDisplayType == FAILSAFE_DISPLAY_USEC);

    updateFailsafe(i);

    if (++col > 7) {
      row += 4;
      col = 0;
    }
  }

  lock = false;
}

void ModulePanel::update()
{
  const PulsesProtocol protocol = (PulsesProtocol)module.protocol;
  const Board::Type board = firmware->getBoard();
  const Multiprotocols::MultiProtocolDefinition & pdef = multiProtocols.getProtocol(module.multi.rfProtocol);
  unsigned int mask = 0;
  unsigned int max_rx_num = 63;

  if (moduleIdx >= 0) {
    mask |= MASK_PROTOCOL;
    switch (protocol) {
      case PULSES_PXX_R9M:
        mask |= MASK_R9M | MASK_SUBTYPES;
      case PULSES_ACCESS_R9M:
      case PULSES_ACCESS_R9M_LITE:
      case PULSES_ACCESS_R9M_LITE_PRO:
      case PULSES_ACCESS_ISRM:
      case PULSES_ACCST_ISRM_D16:
      case PULSES_XJT_LITE_X16:
      case PULSES_XJT_LITE_D8:
      case PULSES_XJT_LITE_LR12:
      case PULSES_PXX_XJT_X16:
      case PULSES_PXX_XJT_D8:
      case PULSES_PXX_XJT_LR12:
      case PULSES_PXX_DJT:
        mask |= MASK_CHANNELS_RANGE | MASK_CHANNELS_COUNT;
        // ACCST Rx ID
        if (protocol==PULSES_PXX_XJT_X16 || protocol==PULSES_PXX_XJT_LR12 ||
            protocol==PULSES_PXX_R9M || protocol==PULSES_ACCST_ISRM_D16 ||
            protocol==PULSES_XJT_LITE_X16 || protocol==PULSES_XJT_LITE_LR12)
          mask |= MASK_RX_NUMBER;
        // ACCESS
        else if (protocol==PULSES_ACCESS_ISRM || protocol==PULSES_ACCESS_R9M ||
                 protocol==PULSES_ACCESS_R9M_LITE || protocol==PULSES_ACCESS_R9M_LITE_PRO)
          mask |= MASK_RX_NUMBER | MASK_ACCESS;
        if (moduleIdx == 0 && HAS_EXTERNAL_ANTENNA(board) && generalSettings.antennaMode == 0 /* per model */)
          mask |= MASK_ANTENNA;
        break;
      case PULSES_LP45:
      case PULSES_DSM2:
      case PULSES_DSMX:
        mask |= MASK_CHANNELS_RANGE | MASK_RX_NUMBER;
        module.channelsCount = 6;
        max_rx_num = 20;
        break;
      case PULSES_CROSSFIRE:
        mask |= MASK_CHANNELS_RANGE;
        module.channelsCount = 16;
        break;
      case PULSES_PPM:
        mask |= MASK_PPM_FIELDS | MASK_SBUSPPM_FIELDS| MASK_CHANNELS_RANGE| MASK_CHANNELS_COUNT;
        if (IS_9XRPRO(board)) {
          mask |= MASK_OPEN_DRAIN;
        }
        break;
      case PULSES_SBUS:
        module.channelsCount = 16;
        mask |=  MASK_SBUSPPM_FIELDS| MASK_CHANNELS_RANGE;
        break;
      case PULSES_MULTIMODULE:
        mask |= MASK_CHANNELS_RANGE | MASK_RX_NUMBER | MASK_MULTIMODULE | MASK_SUBTYPES;
        max_rx_num = 15;
        if (module.multi.rfProtocol == MODULE_SUBTYPE_MULTI_DSM2)
          mask |= MASK_CHANNELS_COUNT;
        else
          module.channelsCount = 16;
        if (pdef.optionsstr != nullptr)
          mask |= MASK_MULTIOPTION;
        if (pdef.hasFailsafe)
          mask |= MASK_FAILSAFES;
        break;
      case PULSES_OFF:
        break;
      default:
        break;
    }
  }
  else if (IS_HORUS_OR_TARANIS(board)) {
    if (model->trainerMode == TRAINER_SLAVE_JACK) {
      mask |= MASK_PPM_FIELDS | MASK_CHANNELS_RANGE | MASK_CHANNELS_COUNT;
    }
  }
  else if (model->trainerMode != TRAINER_MASTER_JACK) {
    mask |= MASK_PPM_FIELDS | MASK_CHANNELS_RANGE | MASK_CHANNELS_COUNT;
  }

  if (moduleHasFailsafes()) {
    mask |= MASK_FAILSAFES;
  }

  ui->label_protocol->setVisible(mask & MASK_PROTOCOL);
  ui->protocol->setVisible(mask & MASK_PROTOCOL);
  ui->label_rxNumber->setVisible(mask & MASK_RX_NUMBER);
  ui->rxNumber->setVisible(mask & MASK_RX_NUMBER);
  ui->rxNumber->setMaximum(max_rx_num);
  ui->rxNumber->setValue(module.modelId);
  ui->label_channelsStart->setVisible(mask & MASK_CHANNELS_RANGE);
  ui->channelsStart->setVisible(mask & MASK_CHANNELS_RANGE);
  ui->channelsStart->setValue(module.channelsStart+1);
  ui->label_channelsCount->setVisible(mask & MASK_CHANNELS_RANGE);
  ui->channelsCount->setVisible(mask & MASK_CHANNELS_RANGE);
  ui->channelsCount->setEnabled(mask & MASK_CHANNELS_COUNT);
  ui->channelsCount->setValue(module.channelsCount);
  ui->channelsCount->setSingleStep(firmware->getCapability(HasPPMStart) ? 1 : 2);

  // PPM settings fields
  ui->label_ppmPolarity->setVisible(mask & MASK_SBUSPPM_FIELDS);
  ui->ppmPolarity->setVisible(mask & MASK_SBUSPPM_FIELDS);
  ui->ppmPolarity->setCurrentIndex(module.ppm.pulsePol);
  ui->label_ppmOutputType->setVisible(mask & MASK_OPEN_DRAIN);
  ui->ppmOutputType->setVisible(mask & MASK_OPEN_DRAIN);
  ui->ppmOutputType->setCurrentIndex(module.ppm.outputType);
  ui->label_ppmDelay->setVisible(mask & MASK_PPM_FIELDS);
  ui->ppmDelay->setVisible(mask & MASK_PPM_FIELDS);
  ui->ppmDelay->setValue(module.ppm.delay);
  ui->label_ppmFrameLength->setVisible(mask & MASK_SBUSPPM_FIELDS);
  ui->ppmFrameLength->setVisible(mask & MASK_SBUSPPM_FIELDS);
  ui->ppmFrameLength->setMinimum(module.channelsCount*(model->extendedLimits ? 2.250 : 2)+3.5);
  ui->ppmFrameLength->setMaximum(firmware->getCapability(PPMFrameLength));
  ui->ppmFrameLength->setValue(22.5+((double)module.ppm.frameLength)*0.5);

  // Antenna mode on Horus and XLite
  if (mask & MASK_ANTENNA) {
    ui->antennaMode->clear();
    ui->antennaMode->addItem(tr("Ask"), -1);
    ui->antennaMode->addItem(tr("Internal"), 0);
    ui->antennaMode->addItem(IS_HORUS_X12S(board) ? tr("Internal + External") : tr("External"), 1);
    ui->antennaMode->setField(module.pxx.antennaMode, this);
  }
  else {
    ui->antennaLabel->hide();
    ui->antennaMode->hide();
  }

  // R9M options
  ui->r9mPower->setVisible(mask & MASK_R9M);
  ui->label_r9mPower->setVisible(mask & MASK_R9M);
  ui->warning_r9mPower->setVisible((mask & MASK_R9M) && module.subType == MODULE_SUBTYPE_R9M_EU);
  ui->warning_r9mFlex->setVisible((mask & MASK_R9M) && module.subType > MODULE_SUBTYPE_R9M_EU);

  if (mask & MASK_R9M) {
    const QSignalBlocker blocker(ui->r9mPower);
    ui->r9mPower->clear();
    ui->r9mPower->addItems(ModuleData::powerValueStrings(module.subType, firmware));
    ui->r9mPower->setCurrentIndex(module.pxx.power);
  }

  // module subtype
  ui->label_multiSubType->setVisible(mask & MASK_SUBTYPES);
  ui->multiSubType->setVisible(mask & MASK_SUBTYPES);
  if (mask & MASK_SUBTYPES) {
    unsigned numEntries = 2;  // R9M FCC/EU
    unsigned i = 0;
    switch(protocol){
    case PULSES_MULTIMODULE:
      numEntries = (module.multi.customProto ? 8 : pdef.numSubTypes());
      break;
    case PULSES_PXX_R9M:
      if (firmware->getCapability(HasModuleR9MFlex))
        i = 2;
      break;
    default:
      break;
    }
    numEntries += i;
    const QSignalBlocker blocker(ui->multiSubType);
    ui->multiSubType->clear();
    for ( ; i < numEntries; i++)
      ui->multiSubType->addItem(module.subTypeToString(i), i);
    ui->multiSubType->setCurrentIndex(ui->multiSubType->findData(module.subType));
  }

  // Multi settings fields
  ui->label_multiProtocol->setVisible(mask & MASK_MULTIMODULE);
  ui->multiProtocol->setVisible(mask & MASK_MULTIMODULE);
  ui->label_option->setVisible(mask & MASK_MULTIOPTION);
  ui->optionValue->setVisible(mask & MASK_MULTIOPTION);
  ui->autoBind->setVisible(mask & MASK_MULTIMODULE);
  ui->lowPower->setVisible(mask & MASK_MULTIMODULE);

  if (mask & MASK_MULTIMODULE) {
    ui->multiProtocol->setCurrentIndex(module.multi.rfProtocol);
    ui->autoBind->setChecked(module.multi.autoBindMode);
    ui->lowPower->setChecked(module.multi.lowPowerMode);
  }

  if (mask & MASK_MULTIOPTION) {
    ui->optionValue->setMinimum(pdef.getOptionMin());
    ui->optionValue->setMaximum(pdef.getOptionMax());
    ui->optionValue->setValue(module.multi.optionValue);
    ui->label_option->setText(qApp->translate("Multiprotocols", qPrintable(pdef.optionsstr)));
  }

  if (mask & MASK_ACCESS) {
    ui->rx1->setText(module.access.receiverName[0]);
    ui->rx2->setText(module.access.receiverName[1]);
    ui->rx3->setText(module.access.receiverName[2]);
  }

  ui->registrationIdLabel->setVisible(mask & MASK_ACCESS);
  ui->registrationId->setVisible(mask & MASK_ACCESS);

  ui->rx1Label->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<0)));
  ui->clearRx1->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<0)));
  ui->rx1->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<0)));

  ui->rx2Label->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<1)));
  ui->clearRx2->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<1)));
  ui->rx2->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<1)));

  ui->rx3Label->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<2)));
  ui->clearRx3->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<2)));
  ui->rx3->setVisible((mask & MASK_ACCESS) && (module.access.receivers & (1<<2)));

  // Failsafes
  ui->label_failsafeMode->setVisible(mask & MASK_FAILSAFES);
  ui->failsafeMode->setVisible(mask & MASK_FAILSAFES);

  if ((mask & MASK_FAILSAFES) && module.failsafeMode == FAILSAFE_CUSTOM) {
    if (ui->failsafesGroupBox->isHidden()) {
      setupFailsafes();
      ui->failsafesGroupBox->setVisible(true);
    }
  }
  else {
    ui->failsafesGroupBox->setVisible(false);
  }

  if (mask & MASK_FAILSAFES) {
    ui->failsafeMode->setCurrentIndex(module.failsafeMode);

    if (firmware->getCapability(ChannelsName)) {
      int chan;
      QString name;
      QMapIterator<int, ChannelFailsafeWidgetsGroup> i(failsafeGroupsMap);
      while (i.hasNext()) {
        i.next();
        chan = i.key();
        name = QString(model->limitData[chan + module.channelsStart].name).trimmed();
        if (name.isEmpty())
          i.value().label->setText(QString::number(chan + 1));
        else
          i.value().label->setText(name);
      }
    }
  }

  if (mask & MASK_CHANNELS_RANGE) {
    ui->channelsStart->setMaximum(33 - ui->channelsCount->value());
    ui->channelsCount->setMaximum(qMin(24, 33-ui->channelsStart->value()));
  }
}

void ModulePanel::on_trainerMode_currentIndexChanged(int index)
{
  if (!lock && model->trainerMode != (unsigned)index) {
    model->trainerMode = index;
    update();
    emit modified();
  }
}

void ModulePanel::onProtocolChanged(int index)
{
  if (!lock && module.protocol != ui->protocol->itemData(index).toUInt()) {
    module.protocol = ui->protocol->itemData(index).toInt();
    update();
    emit modified();
  }
}

void ModulePanel::on_ppmPolarity_currentIndexChanged(int index)
{
  if (!lock && module.ppm.pulsePol != (bool)index) {
    module.ppm.pulsePol = index;
    emit modified();
  }
}

void ModulePanel::on_r9mPower_currentIndexChanged(int index)
{
  if (!lock && module.pxx.power != (unsigned int)index) {
    module.pxx.power = index;
    emit modified();
  }
}

void ModulePanel::on_ppmOutputType_currentIndexChanged(int index)
{
  if (!lock && module.ppm.outputType != (bool)index) {
    module.ppm.outputType = index;
    emit modified();
  }
}

void ModulePanel::on_channelsCount_editingFinished()
{
  if (!lock && module.channelsCount != ui->channelsCount->value()) {
    module.channelsCount = ui->channelsCount->value();
    update();
    emit channelsRangeChanged();
    emit modified();
  }
}

void ModulePanel::on_channelsStart_editingFinished()
{
  if (!lock && module.channelsStart != (unsigned)ui->channelsStart->value() - 1) {
    module.channelsStart = (unsigned)ui->channelsStart->value() - 1;
    update();
    emit channelsRangeChanged();
    emit modified();
  }
}

void ModulePanel::on_ppmDelay_editingFinished()
{
  if (!lock && module.ppm.delay != ui->ppmDelay->value()) {
    // TODO only accept valid values
    module.ppm.delay = ui->ppmDelay->value();
    emit modified();
  }
}

void ModulePanel::on_rxNumber_editingFinished()
{
  if (module.modelId != (unsigned)ui->rxNumber->value()) {
    module.modelId = (unsigned)ui->rxNumber->value();
    emit modified();
  }
}

void ModulePanel::on_ppmFrameLength_editingFinished()
{
  int val = (ui->ppmFrameLength->value()-22.5) / 0.5;
  if (module.ppm.frameLength != val) {
    module.ppm.frameLength = val;
    emit modified();
  }
}

void ModulePanel::on_failsafeMode_currentIndexChanged(int value)
{
  if (!lock && module.failsafeMode != (unsigned)value) {
    module.failsafeMode = value;
    update();
    emit modified();
  }
}

void ModulePanel::onMultiProtocolChanged(int index)
{
  if (!lock && module.multi.rfProtocol != (unsigned)index) {
    lock=true;
    module.multi.rfProtocol = (unsigned int) index;
    unsigned int maxSubTypes = multiProtocols.getProtocol(index).numSubTypes();
    if (module.multi.customProto)
      maxSubTypes=8;
    module.subType = std::min(module.subType, maxSubTypes -1);
    update();
    emit modified();
    lock = false;
  }
}

void ModulePanel::on_optionValue_editingFinished()
{
  if (module.multi.optionValue != ui->optionValue->value()) {
    module.multi.optionValue = ui->optionValue->value();
    emit modified();
  }
}

void ModulePanel::onSubTypeChanged()
{
  const unsigned type = ui->multiSubType->currentData().toUInt();
  if (!lock && module.subType != type) {
    lock=true;
    module.subType = type;
    update();
    emit modified();
    lock =  false;
  }
}

void ModulePanel::on_autoBind_stateChanged(int state)
{
  module.multi.autoBindMode = (state == Qt::Checked);
}
void ModulePanel::on_lowPower_stateChanged(int state)
{
  module.multi.lowPowerMode = (state == Qt::Checked);
}

// updtSb (update spin box(es)): 0=none or bitmask of FailsafeValueDisplayTypes
void ModulePanel::setChannelFailsafeValue(const int channel, const int value, quint8 updtSb)
{
  if (channel < 0 || channel >= CPN_MAX_CHNOUT)
    return;

  module.failsafeChannels[channel] = value;
  double pctVal = divRoundClosest(value * 1000, 1024) / 10.0;
  // qDebug() << value << pctVal;

  if (failsafeGroupsMap.contains(channel)) {
    const ChannelFailsafeWidgetsGroup & grp = failsafeGroupsMap.value(channel);
    if ((updtSb & FAILSAFE_DISPLAY_PERCENT) && grp.sbPercent) {
      grp.sbPercent->blockSignals(true);
      grp.sbPercent->setValue(pctVal);
      grp.sbPercent->blockSignals(false);
    }
    if ((updtSb & FAILSAFE_DISPLAY_USEC) && grp.sbUsec) {
      grp.sbUsec->blockSignals(true);
      grp.sbUsec->setValue(value);
      grp.sbUsec->blockSignals(false);
    }
  }
  if (!lock)
    emit modified();
}

void ModulePanel::onFailsafeUsecChanged(int value)
{
  if (!sender())
    return;

  bool ok = false;
  int channel = sender()->property("index").toInt(&ok);
  if (ok)
    setChannelFailsafeValue(channel, value, FAILSAFE_DISPLAY_PERCENT);
}

void ModulePanel::onFailsafePercentChanged(double value)
{
  if (!sender())
    return;

  bool ok = false;
  int channel = sender()->property("index").toInt(&ok);
  if (ok)
    setChannelFailsafeValue(channel, divRoundClosest(int(value * 1024), 100), FAILSAFE_DISPLAY_USEC);
}

void ModulePanel::onFailsafeComboIndexChanged(int index)
{
  if (!sender())
    return;

  QComboBox * cb = qobject_cast<QComboBox *>(sender());

  if (cb && !lock) {
    lock = true;
    bool ok = false;
    int channel = sender()->property("index").toInt(&ok);
    if (ok) {
      module.failsafeChannels[channel] = cb->itemData(index).toInt();
      updateFailsafe(channel);
      emit modified();
    }
    lock = false;
  }
}

void ModulePanel::onFailsafesDisplayValueTypeChanged(int type)
{
  if (failsafesValueDisplayType != type) {
    failsafesValueDisplayType = type;
    foreach (ChannelFailsafeWidgetsGroup grp, failsafeGroupsMap) {
      if (grp.sbPercent)
        grp.sbPercent->setVisible(type == FAILSAFE_DISPLAY_PERCENT);
      if (grp.sbUsec)
        grp.sbUsec->setVisible(type == FAILSAFE_DISPLAY_USEC);
    }
  }
}

void ModulePanel::onExtendedLimitsToggled()
{
  double channelMaxPct = double(model->getChannelsMax());
  int channelMaxUs = 512 * channelMaxPct / 100 * 2;
  foreach (ChannelFailsafeWidgetsGroup grp, failsafeGroupsMap) {
    if (grp.sbPercent)
      grp.sbPercent->setRange(-channelMaxPct, channelMaxPct);
    if (grp.sbUsec)
      grp.sbUsec->setRange(-channelMaxUs, channelMaxUs);
  }
}

void ModulePanel::updateFailsafe(int channel)
{
  if (channel >= CPN_MAX_CHNOUT || !failsafeGroupsMap.contains(channel))
    return;

  const int failsafeValue = module.failsafeChannels[channel];
  const ChannelFailsafeWidgetsGroup & grp = failsafeGroupsMap.value(channel);
  const bool valDisable = (failsafeValue == FAILSAFE_CHANNEL_HOLD || failsafeValue == FAILSAFE_CHANNEL_NOPULSE);

  if (grp.combo)
    grp.combo->setCurrentIndex(grp.combo->findData(valDisable ? failsafeValue : 0));
  if (grp.sbPercent)
    grp.sbPercent->setDisabled(valDisable);
  if (grp.sbUsec)
    grp.sbUsec->setDisabled(valDisable);

  if (!valDisable)
    setChannelFailsafeValue(channel, failsafeValue, (FAILSAFE_DISPLAY_PERCENT | FAILSAFE_DISPLAY_USEC));
}

void ModulePanel::onClearAccessRxClicked()
{
  QPushButton *button = qobject_cast<QPushButton *>(sender());

  if (button == ui->clearRx1) {
    module.access.receivers &= ~(1<<0);
    ui->rx1->clear();
    update();
    emit modified();
  }
  else if (button == ui->clearRx2) {
    module.access.receivers &= ~(1<<1);
    ui->rx2->clear();
    update();
    emit modified();
  }
  else if (button == ui->clearRx3) {
    module.access.receivers &= ~(1<<2);
    ui->rx3->clear();
    update();
    emit modified();
  }
}

/******************************************************************************/

SetupPanel::SetupPanel(QWidget * parent, ModelData & model, GeneralSettings & generalSettings, Firmware * firmware):
  ModelPanel(parent, model, generalSettings, firmware),
  ui(new Ui::Setup)
{
  Board::Type board = firmware->getBoard();

  lock = true;

  memset(modules, 0, sizeof(modules));

  ui->setupUi(this);

  QRegExp rx(CHAR_FOR_NAMES_REGEX);
  ui->name->setValidator(new QRegExpValidator(rx, this));
  ui->name->setMaxLength(firmware->getCapability(ModelName));

  if (firmware->getCapability(ModelImage)) {
    QStringList items;
    items.append("");
    QString path = g.profile[g.id()].sdPath();
    path.append("/IMAGES/");
    QDir qd(path);
    if (qd.exists()) {
      QStringList filters;
      if(IS_HORUS(board)) {
        filters << "*.bmp" << "*.jpg" << "*.png";
        foreach ( QString file, qd.entryList(filters, QDir::Files) ) {
          QFileInfo fi(file);
          QString temp = fi.fileName();
          if (!items.contains(temp) && temp.length() <= 6+4) {
            items.append(temp);
          }
        }
      }
      else {
        filters << "*.bmp";
        foreach (QString file, qd.entryList(filters, QDir::Files)) {
          QFileInfo fi(file);
          QString temp = fi.completeBaseName();
          if (!items.contains(temp) && temp.length() <= 10+4) {
            items.append(temp);
          }
        }
      }
    }
    if (!items.contains(model.bitmap)) {
      items.append(model.bitmap);
    }
    items.sort();
    foreach (QString file, items) {
      ui->image->addItem(file);
      if (file == model.bitmap) {
        ui->image->setCurrentIndex(ui->image->count()-1);
        QString fileName = path;
        fileName.append(model.bitmap);
        if (!IS_HORUS(board))
          fileName.append(".bmp");
        QImage image(fileName);
        if (image.isNull() && !IS_HORUS(board)) {
          fileName = path;
          fileName.append(model.bitmap);
          fileName.append(".BMP");
          image.load(fileName);
        }
        if (!image.isNull()) {
          if (IS_HORUS(board)) {
            ui->imagePreview->setFixedSize(QSize(192, 114));
            ui->imagePreview->setPixmap(QPixmap::fromImage(image.scaled(192, 114)));
          }
          else {
            ui->imagePreview->setFixedSize(QSize(64, 32));
            ui->imagePreview->setPixmap(QPixmap::fromImage(image.scaled(64, 32)));
          }
        }
      }
    }
  }
  else {
    ui->image->hide();
    ui->modelImage_label->hide();
    ui->imagePreview->hide();
  }

  QWidget * prevFocus = ui->image;
  RawSwitchFilterItemModel * swModel = new RawSwitchFilterItemModel(&generalSettings, &model, RawSwitch::TimersContext, this);
  connect(this, &SetupPanel::updated, swModel, &RawSwitchFilterItemModel::update);

  for (int i=0; i<CPN_MAX_TIMERS; i++) {
    if (i<firmware->getCapability(Timers)) {
      timers[i] = new TimerPanel(this, model, model.timers[i], generalSettings, firmware, prevFocus, swModel);
      ui->gridLayout->addWidget(timers[i], 1+i, 1);
      connect(timers[i], &TimerPanel::modified, this, &SetupPanel::modified);
      connect(this, &SetupPanel::updated, timers[i], &TimerPanel::update);
      prevFocus = timers[i]->getLastFocus();
    }
    else {
      foreach(QLabel *label, findChildren<QLabel *>(QRegularExpression(QString("label_timer%1").arg(i+1)))) {
        label->hide();
      }
    }
  }

  if (firmware->getCapability(HasTopLcd)) {
    ui->toplcdTimer->setField(model.toplcdTimer, this);
    for (int i=0; i<CPN_MAX_TIMERS; i++) {
      if (i<firmware->getCapability(Timers)) {
        ui->toplcdTimer->addItem(tr("Timer %1").arg(i+1), i);
      }
    }
  }
  else {
    ui->toplcdTimerLabel->hide();
    ui->toplcdTimer->hide();
  }

  if (!firmware->getCapability(HasDisplayText)) {
    ui->displayText->hide();
    ui->editText->hide();
  }

  if (!firmware->getCapability(GlobalFunctions)) {
    ui->gfEnabled->hide();
  }

  // Beep Center checkboxes
  prevFocus = ui->trimsDisplay;
  int analogs = CPN_MAX_STICKS + getBoardCapability(board, Board::Pots) + getBoardCapability(board, Board::Sliders);
  int genAryIdx = 0;
  for (int i=0; i < analogs + firmware->getCapability(RotaryEncoders); i++) {
    RawSource src((i < analogs) ? SOURCE_TYPE_STICK : SOURCE_TYPE_ROTARY_ENCODER, (i < analogs) ? i : analogs - i);
    QCheckBox * checkbox = new QCheckBox(this);
    checkbox->setProperty("index", i);
    checkbox->setText(src.toString(&model, &generalSettings));
    ui->centerBeepLayout->addWidget(checkbox, 0, i+1);
    connect(checkbox, SIGNAL(toggled(bool)), this, SLOT(onBeepCenterToggled(bool)));
    centerBeepCheckboxes << checkbox;
    if (IS_HORUS_OR_TARANIS(board)) {
      if (src.isPot(&genAryIdx) && !generalSettings.isPotAvailable(genAryIdx)) {
        checkbox->hide();
      }
      else if (src.isSlider(&genAryIdx) && !generalSettings.isSliderAvailable(genAryIdx)) {
        checkbox->hide();
      }
    }
    QWidget::setTabOrder(prevFocus, checkbox);
    prevFocus = checkbox;
  }

  // Startup switches warnings
  for (int i=0; i<getBoardCapability(board, Board::Switches); i++) {
    Board::SwitchInfo switchInfo = Boards::getSwitchInfo(board, i);
    switchInfo.config = Board::SwitchType(generalSettings.switchConfig[i]);
    if (switchInfo.config == Board::SWITCH_NOT_AVAILABLE || switchInfo.config == Board::SWITCH_TOGGLE) {
      continue;
    }
    RawSource src(RawSourceType::SOURCE_TYPE_SWITCH, i);
    QLabel * label = new QLabel(this);
    QSlider * slider = new QSlider(this);
    QCheckBox * cb = new QCheckBox(this);
    slider->setProperty("index", i);
    slider->setOrientation(Qt::Vertical);
    slider->setMinimum(0);
    slider->setInvertedAppearance(true);
    slider->setInvertedControls(true);
    slider->setTickPosition(QSlider::TicksBothSides);
    slider->setMinimumSize(QSize(30, 50));
    slider->setMaximumSize(QSize(50, 50));
    slider->setSingleStep(1);
    slider->setPageStep(1);
    slider->setTickInterval(1);
    label->setText(src.toString(&model, &generalSettings));
    slider->setMaximum(switchInfo.config == Board::SWITCH_3POS ? 2 : 1);
    cb->setProperty("index", i);
    ui->switchesStartupLayout->addWidget(label, 0, i+1);
    ui->switchesStartupLayout->setAlignment(label, Qt::AlignCenter);
    ui->switchesStartupLayout->addWidget(slider, 1, i+1);
    ui->switchesStartupLayout->setAlignment(slider, Qt::AlignCenter);
    ui->switchesStartupLayout->addWidget(cb, 2, i+1);
    ui->switchesStartupLayout->setAlignment(cb, Qt::AlignCenter);
    connect(slider, SIGNAL(valueChanged(int)), this, SLOT(startupSwitchEdited(int)));
    connect(cb, SIGNAL(toggled(bool)), this, SLOT(startupSwitchToggled(bool)));
    startupSwitchesSliders << slider;
    startupSwitchesCheckboxes << cb;
    QWidget::setTabOrder(prevFocus, slider);
    QWidget::setTabOrder(slider, cb);
    prevFocus = cb;
  }

  // Pot warnings
  prevFocus = ui->potWarningMode;
  if (IS_HORUS_OR_TARANIS(board)) {
    for (int i=0; i<getBoardCapability(board, Board::Pots)+getBoardCapability(board, Board::Sliders); i++) {
      RawSource src(SOURCE_TYPE_STICK, CPN_MAX_STICKS + i);
      QCheckBox * cb = new QCheckBox(this);
      cb->setProperty("index", i);
      cb->setText(src.toString(&model, &generalSettings));
      ui->potWarningLayout->addWidget(cb, 0, i+1);
      connect(cb, SIGNAL(toggled(bool)), this, SLOT(potWarningToggled(bool)));
      potWarningCheckboxes << cb;
      if (src.isPot(&genAryIdx) && !generalSettings.isPotAvailable(genAryIdx)) {
        cb->hide();
      }
      else if (src.isSlider(&genAryIdx) && !generalSettings.isSliderAvailable(genAryIdx)) {
        cb->hide();
      }
      QWidget::setTabOrder(prevFocus, cb);
      prevFocus = cb;
    }
  }
  else {
    ui->label_potWarning->hide();
    ui->potWarningMode->hide();
  }

  if (IS_ARM(board)) {
    ui->trimsDisplay->setField(model.trimsDisplay, this);
  }
  else {
    ui->labelTrimsDisplay->hide();
    ui->trimsDisplay->hide();
  }

  for (int i=firmware->getCapability(NumFirstUsableModule); i<firmware->getCapability(NumModules); i++) {
    modules[i] = new ModulePanel(this, model, model.moduleData[i], generalSettings, firmware, i);
    ui->modulesLayout->addWidget(modules[i]);
    connect(modules[i], &ModulePanel::modified, this, &SetupPanel::modified);
    connect(this, &SetupPanel::extendedLimitsToggled, modules[i], &ModulePanel::onExtendedLimitsToggled);
  }

  if (firmware->getCapability(ModelTrainerEnable)) {
    modules[CPN_MAX_MODULES] = new ModulePanel(this, model, model.moduleData[CPN_MAX_MODULES], generalSettings, firmware, -1);
    ui->modulesLayout->addWidget(modules[CPN_MAX_MODULES]);
    connect(modules[CPN_MAX_MODULES], &ModulePanel::modified, this, &SetupPanel::modified);
  }

  disableMouseScrolling();

  lock = false;
}

SetupPanel::~SetupPanel()
{
  delete ui;
}

void SetupPanel::on_extendedLimits_toggled(bool checked)
{
  model->extendedLimits = checked;
  emit extendedLimitsToggled();
  emit modified();
}

void SetupPanel::on_throttleWarning_toggled(bool checked)
{
  model->disableThrottleWarning = !checked;
  emit modified();
}

void SetupPanel::on_throttleReverse_toggled(bool checked)
{
  model->throttleReversed = checked;
  emit modified();
}

void SetupPanel::on_extendedTrims_toggled(bool checked)
{
  model->extendedTrims = checked;
  emit modified();
}

void SetupPanel::on_trimIncrement_currentIndexChanged(int index)
{
  model->trimInc = index-2;
  emit modified();
}

void SetupPanel::on_throttleSource_currentIndexChanged(int index)
{
  if (!lock) {
    model->thrTraceSrc = ui->throttleSource->currentData().toUInt();
    emit modified();
  }
}

void SetupPanel::on_name_editingFinished()
{
  if (QString(model->name) != ui->name->text()) {
    int length = ui->name->maxLength();
    strncpy(model->name, ui->name->text().toLatin1(), length);
    emit modified();
  }
}

void SetupPanel::on_image_currentIndexChanged(int index)
{
  if (!lock) {
    Board::Type board = firmware->getBoard();
    strncpy(model->bitmap, ui->image->currentText().toLatin1(), 10);
    QString path = g.profile[g.id()].sdPath();
    path.append("/IMAGES/");
    QDir qd(path);
    if (qd.exists()) {
      QString fileName=path;
      fileName.append(model->bitmap);
      if (!IS_HORUS(board))
        fileName.append(".bmp");
      QImage image(fileName);
      if (image.isNull() && !IS_HORUS(board)) {
        fileName=path;
        fileName.append(model->bitmap);
        fileName.append(".BMP");
        image.load(fileName);
      }
      if (!image.isNull()) {
        if (IS_HORUS(board)) {
          ui->imagePreview->setFixedSize(QSize(192, 114));
          ui->imagePreview->setPixmap(QPixmap::fromImage(image.scaled(192, 114)));
        }
        else {
          ui->imagePreview->setFixedSize(QSize(64, 32));
          ui->imagePreview->setPixmap(QPixmap::fromImage(image.scaled(64, 32)));
        }
      }
      else {
        ui->imagePreview->clear();
      }
    }
    else {
      ui->imagePreview->clear();
    }
    emit modified();
  }
}

void SetupPanel::populateThrottleSourceCB()
{
  Board::Type board = firmware->getBoard();
  lock = true;
  ui->throttleSource->clear();
  ui->throttleSource->addItem(tr("THR"), 0);

  int idx=1;
  for (int i=0; i<getBoardCapability(board, Board::Pots)+getBoardCapability(board, Board::Sliders); i++, idx++) {
    if (RawSource(SOURCE_TYPE_STICK,4+i).isAvailable(model,&generalSettings,board)) {
      ui->throttleSource->addItem(firmware->getAnalogInputName(4+i), idx);
    }
  }
  for (int i=0; i<firmware->getCapability(Outputs); i++, idx++) {
    ui->throttleSource->addItem(RawSource(SOURCE_TYPE_CH, i).toString(model, &generalSettings), idx);
  }

  int thrTraceSrcIdx = ui->throttleSource->findData(model->thrTraceSrc);
  ui->throttleSource->setCurrentIndex(thrTraceSrcIdx);
  lock = false;
}

void SetupPanel::update()
{
  ui->name->setText(model->name);
  ui->throttleReverse->setChecked(model->throttleReversed);
  populateThrottleSourceCB();
  ui->throttleWarning->setChecked(!model->disableThrottleWarning);
  ui->trimIncrement->setCurrentIndex(model->trimInc+2);
  ui->throttleTrim->setChecked(model->thrTrim);
  ui->extendedLimits->setChecked(model->extendedLimits);
  ui->extendedTrims->setChecked(model->extendedTrims);
  ui->displayText->setChecked(model->displayChecklist);
  ui->gfEnabled->setChecked(!model->noGlobalFunctions);

  updateBeepCenter();
  updateStartupSwitches();

  if (IS_HORUS_OR_TARANIS(firmware->getBoard())) {
    updatePotWarnings();
  }

  for (int i=0; i<CPN_MAX_MODULES+1; i++) {
    if (modules[i]) {
      modules[i]->update();
    }
  }

  emit updated();
}

void SetupPanel::updateBeepCenter()
{
  for (int i=0; i<centerBeepCheckboxes.size(); i++) {
    centerBeepCheckboxes[i]->setChecked(model->beepANACenter & (0x01 << i));
  }
}

void SetupPanel::updateStartupSwitches()
{
  lock = true;

  uint64_t switchStates = model->switchWarningStates;
  uint64_t value;

  for (int i=0; i<startupSwitchesSliders.size(); i++) {
    QSlider * slider = startupSwitchesSliders[i];
    QCheckBox * cb = startupSwitchesCheckboxes[i];
    int index = slider->property("index").toInt();
    bool enabled = !(model->switchWarningEnable & (1 << index));
    if (IS_HORUS_OR_TARANIS(firmware->getBoard())) {
      value = (switchStates >> (2*index)) & 0x03;
      if (generalSettings.switchConfig[index] != Board::SWITCH_3POS && value == 2) {
        value = 1;
      }
    }
    else {
      value = (i==0 ? switchStates & 0x3 : switchStates & 0x1);
      switchStates >>= (i==0 ? 2 : 1);
    }
    slider->setValue(value);
    slider->setEnabled(enabled);
    cb->setChecked(enabled);
  }

  lock = false;
}

void SetupPanel::startupSwitchEdited(int value)
{
  if (!lock) {
    int shift = 0;
    uint64_t mask;
    int index = sender()->property("index").toInt();

    if (IS_HORUS_OR_TARANIS(firmware->getBoard())) {
      shift = index * 2;
      mask = 0x03ull << shift;
    }
    else {
      if (index == 0) {
        mask = 0x03;
      }
      else {
        shift = index+1;
        mask = 0x01ull << shift;
      }
    }

    model->switchWarningStates &= ~mask;

    if (IS_HORUS_OR_TARANIS(firmware->getBoard()) && generalSettings.switchConfig[index] != Board::SWITCH_3POS) {
      if (value == 1) {
        value = 2;
      }
    }

    if (value) {
      model->switchWarningStates |= ((uint64_t)value << shift);
    }

    updateStartupSwitches();
    emit modified();
  }
}

void SetupPanel::startupSwitchToggled(bool checked)
{
  if (!lock) {
    int index = sender()->property("index").toInt();

    if (checked)
      model->switchWarningEnable &= ~(1 << index);
    else
      model->switchWarningEnable |= (1 << index);

    updateStartupSwitches();
    emit modified();
  }
}

void SetupPanel::updatePotWarnings()
{
  lock = true;
  ui->potWarningMode->setCurrentIndex(model->potsWarningMode);
  for (int i=0; i<potWarningCheckboxes.size(); i++) {
    QCheckBox *checkbox = potWarningCheckboxes[i];
    int index = checkbox->property("index").toInt();
    checkbox->setChecked(!model->potsWarnEnabled[index]);
    checkbox->setDisabled(model->potsWarningMode == 0);
  }
  lock = false;
}

void SetupPanel::potWarningToggled(bool checked)
{
  if (!lock) {
    int index = sender()->property("index").toInt();
    model->potsWarnEnabled[index] = !checked;
    updatePotWarnings();
    emit modified();
  }
}

void SetupPanel::on_potWarningMode_currentIndexChanged(int index)
{
  if (!lock) {
    model->potsWarningMode = index;
    updatePotWarnings();
    emit modified();
  }
}

void SetupPanel::on_displayText_toggled(bool checked)
{
  model->displayChecklist = checked;
  emit modified();
}

void SetupPanel::on_gfEnabled_toggled(bool checked)
{
  model->noGlobalFunctions = !checked;
  emit modified();
}

void SetupPanel::on_throttleTrim_toggled(bool checked)
{
  model->thrTrim = checked;
  emit modified();
}

void SetupPanel::onBeepCenterToggled(bool checked)
{
  if (!lock) {
    int index = sender()->property("index").toInt();
    unsigned int mask = (0x01 << index);
    if (checked)
      model->beepANACenter |= mask;
    else
      model->beepANACenter &= ~mask;
    emit modified();
  }
}

void SetupPanel::on_editText_clicked()
{
  const QString path = Helpers::getChecklistsPath();
  QDir d(path);
  if (!d.exists()) {
    QMessageBox::critical(this, tr("Profile Settings"), tr("SD structure path not specified or invalid"));
  }
  else {
    ChecklistDialog *g = new ChecklistDialog(this, model);
    g->exec();
  }
}

