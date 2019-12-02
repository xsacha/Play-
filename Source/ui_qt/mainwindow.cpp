#include "mainwindow.h"
#include "QStringUtils.h"
#include "settingsdialog.h"
#include "memorycardmanagerdialog.h"
#include "S3FileBrowser.h"
#include "ui_shared/BootablesProcesses.h"
#include "ui_shared/StatsManager.h"

#define VULKAN

#ifdef VULKAN
#include "vulkanwindow.h"
#include "GSH_VulkanQt.h"
#else
#include "openglwindow.h"
#include "GSH_OpenGLQt.h"
#endif

#include <QDateTime>
#include <QFileDialog>
#include <QTimer>
#include <QWindow>
#include <QMessageBox>
#include <QStorageInfo>
#include <ctime>

#include "StdStreamUtils.h"
#include "string_format.h"

#ifdef _WIN32
#include "../../tools/PsfPlayer/Source/win32_ui/SH_WaveOut.h"
#ifdef DEBUGGER_INCLUDED
#include "win32/DebugSupport/Debugger.h"
#include "win32/DebugSupport/FrameDebugger/FrameDebugger.h"
#include "ui_debugmenu.h"
#endif
#else
#include "tools/PsfPlayer/Source/SH_OpenAL.h"
#endif
#include "input/PH_GenericInput.h"
#include "DiskUtils.h"
#include "PathUtils.h"
#include <zlib.h>

#include "CoverUtils.h"
#include "PreferenceDefs.h"
#include "PS2VM_Preferences.h"
#include "ScreenShotUtils.h"

#include "ui_mainwindow.h"
#include "vfsmanagerdialog.h"
#include "bootablelistdialog.h"
#include "ControllerConfig/controllerconfigdialog.h"

#ifdef __APPLE__
#include "macos/InputProviderMacOsHid.h"
#endif
#ifdef HAS_LIBEVDEV
#include "unix/InputProviderEvDev.h"
#endif
#ifdef WIN32
#include "win32/InputProviderDirectInput.h"
#include "win32/InputProviderXInput.h"
#endif

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	m_continuationChecker = new CContinuationChecker(this);

#ifdef VULKAN
	m_outputwindow = new VulkanWindow;
#else
	m_outputwindow = new OpenGLWindow;
#endif
	QWidget* container = QWidget::createWindowContainer(m_outputwindow);
	ui->gridLayout->addWidget(container, 0, 0);

	connect(m_outputwindow, SIGNAL(heightChanged(int)), this, SLOT(outputWindow_resized()));
	connect(m_outputwindow, SIGNAL(widthChanged(int)), this, SLOT(outputWindow_resized()));

	connect(m_outputwindow, SIGNAL(keyUp(QKeyEvent*)), this, SLOT(keyReleaseEvent(QKeyEvent*)));
	connect(m_outputwindow, SIGNAL(keyDown(QKeyEvent*)), this, SLOT(keyPressEvent(QKeyEvent*)));

	connect(m_outputwindow, SIGNAL(focusOut(QFocusEvent*)), this, SLOT(focusOutEvent(QFocusEvent*)));
	connect(m_outputwindow, SIGNAL(focusIn(QFocusEvent*)), this, SLOT(focusInEvent(QFocusEvent*)));

	connect(m_outputwindow, SIGNAL(doubleClick(QMouseEvent*)), this, SLOT(doubleClickEvent(QMouseEvent*)));

#ifdef PROFILE
	{
		m_profileStatsLabel = new QLabel(this);
		QFont courierFont("Courier");
		m_profileStatsLabel->setFont(courierFont);
		m_profileStatsLabel->setAlignment(Qt::AlignTop);
		ui->gridLayout->addWidget(m_profileStatsLabel, 0, 1);
	}
#endif

	RegisterPreferences();

	m_pauseFocusLost = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST);
	auto lastPath = CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_CDROM0_PATH);
	std::error_code lastPathExistsErrorCode;
	if(fs::exists(lastPath, lastPathExistsErrorCode))
	{
		m_lastPath = lastPath.parent_path();
	}
	else
	{
		m_lastPath = QStringToPath(QDir::homePath());
	}

	CreateStatusBar();
	UpdateUI();
	ui->actionBoot_DiscImage_S3->setVisible(S3FileBrowser::IsAvailable());

	InitVirtualMachine();

#ifdef DEBUGGER_INCLUDED
	m_debugger = std::make_unique<CDebugger>(*m_virtualMachine);
	m_frameDebugger = std::make_unique<CFrameDebugger>();

	auto debugMenu = new QMenu(this);
	debugMenuUi = new Ui::DebugMenu();
	debugMenuUi->setupUi(debugMenu);
	ui->menuBar->insertMenu(ui->menuHelp->menuAction(), debugMenu);

	connect(debugMenuUi->actionShowDebugger, &QAction::triggered, this, std::bind(&MainWindow::ShowDebugger, this));
	connect(debugMenuUi->actionShowFrameDebugger, &QAction::triggered, this, std::bind(&MainWindow::ShowFrameDebugger, this));
	connect(debugMenuUi->actionDumpNextFrame, &QAction::triggered, this, std::bind(&MainWindow::DumpNextFrame, this));
	connect(debugMenuUi->actionGsDrawEnabled, &QAction::triggered, this, std::bind(&MainWindow::ToggleGsDraw, this));
#endif
}

MainWindow::~MainWindow()
{
#ifdef DEBUGGER_INCLUDED
	m_debugger.reset();
	m_frameDebugger.reset();
	delete debugMenuUi;
#endif
	CAppConfig::GetInstance().Save();
	if(m_virtualMachine != nullptr)
	{
		m_virtualMachine->Pause();
		m_qtKeyInputProvider.reset();
		m_virtualMachine->DestroyPadHandler();
		m_virtualMachine->DestroyGSHandler();
		m_virtualMachine->DestroySoundHandler();
		m_virtualMachine->Destroy();
		delete m_virtualMachine;
		m_virtualMachine = nullptr;
	}
	delete ui;
}

void MainWindow::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	SetupGsHandler();
}

void MainWindow::InitVirtualMachine()
{
	assert(!m_virtualMachine);

	m_virtualMachine = new CPS2VM();
	m_virtualMachine->Initialize();

	SetupSoundHandler();

	{
		m_virtualMachine->CreatePadHandler(CPH_GenericInput::GetFactoryFunction());
		auto padHandler = static_cast<CPH_GenericInput*>(m_virtualMachine->GetPadHandler());
		auto& bindingManager = padHandler->GetBindingManager();

		//Create QtKeyInputProvider
		m_qtKeyInputProvider = std::make_shared<CInputProviderQtKey>();
		bindingManager.RegisterInputProvider(m_qtKeyInputProvider);
#ifdef __APPLE__
		bindingManager.RegisterInputProvider(std::make_shared<CInputProviderMacOsHid>());
#endif
#ifdef HAS_LIBEVDEV
		bindingManager.RegisterInputProvider(std::make_shared<CInputProviderEvDev>());
#endif
#ifdef WIN32
		bindingManager.RegisterInputProvider(std::make_shared<CInputProviderDirectInput>());
		bindingManager.RegisterInputProvider(std::make_shared<CInputProviderXInput>());
#endif
		if(!bindingManager.HasBindings())
		{
			ControllerConfigDialog::AutoConfigureKeyboard(&bindingManager);
		}
	}

#ifdef PROFILE
	m_profileFrameDoneConnection = m_virtualMachine->ProfileFrameDone.Connect(std::bind(&CStatsManager::OnProfileFrameDone, &CStatsManager::GetInstance(), m_virtualMachine, std::placeholders::_1));
#endif

	//OnExecutableChange might be called from another thread, we need to wrap it around a Qt signal
	m_OnExecutableChangeConnection = m_virtualMachine->m_ee->m_os->OnExecutableChange.Connect(std::bind(&MainWindow::EmitOnExecutableChange, this));
	connect(this, SIGNAL(onExecutableChange()), this, SLOT(HandleOnExecutableChange()));
}

void MainWindow::SetOutputWindowSize()
{
	outputWindow_resized();
}

void MainWindow::SetupGsHandler()
{
	assert(m_virtualMachine);
	auto gsHandler = m_virtualMachine->GetGSHandler();
	if(!gsHandler)
	{
#ifdef VULKAN
		m_virtualMachine->CreateGSHandler(CGSH_VulkanQt::GetFactoryFunction(m_outputwindow));
#else
		m_virtualMachine->CreateGSHandler(CGSH_OpenGLQt::GetFactoryFunction(m_outputwindow));
#endif
		m_OnNewFrameConnection = m_virtualMachine->m_ee->m_gs->OnNewFrame.Connect(std::bind(&CStatsManager::OnNewFrame, &CStatsManager::GetInstance(), std::placeholders::_1));
	}
}

void MainWindow::SetupSoundHandler()
{
	assert(m_virtualMachine);
	bool audioEnabled = CAppConfig::GetInstance().GetPreferenceBoolean(PREFERENCE_AUDIO_ENABLEOUTPUT);
	if(audioEnabled)
	{
#ifdef _WIN32
		m_virtualMachine->CreateSoundHandler(&CSH_WaveOut::HandlerFactory);
#else
		m_virtualMachine->CreateSoundHandler(&CSH_OpenAL::HandlerFactory);
#endif
	}
	else
	{
		m_virtualMachine->DestroySoundHandler();
	}
}

void MainWindow::outputWindow_resized()
{
	if(m_virtualMachine != nullptr && m_virtualMachine->m_ee != nullptr && m_virtualMachine->m_ee->m_gs != nullptr)
	{
		uint32 w = m_outputwindow->size().width(), h = m_outputwindow->size().height();

		auto scale = devicePixelRatioF();
		CGSHandler::PRESENTATION_PARAMS presentationParams;
		presentationParams.mode = static_cast<CGSHandler::PRESENTATION_MODE>(CAppConfig::GetInstance().GetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE));
		presentationParams.windowWidth = w * scale;
		presentationParams.windowHeight = h * scale;
		m_virtualMachine->m_ee->m_gs->SetPresentationParams(presentationParams);
		m_virtualMachine->m_ee->m_gs->Flip(true);
	}
}

void MainWindow::on_actionBoot_DiscImage_triggered()
{
	QFileDialog dialog(this);
	dialog.setDirectory(PathToQString(m_lastPath));
	dialog.setFileMode(QFileDialog::ExistingFile);
	dialog.setNameFilter(tr("All supported types(*.iso *.bin *.isz *.cso);;UltraISO Compressed Disk Images (*.isz);;CISO Compressed Disk Images (*.cso);;All files (*.*)"));
	if(dialog.exec())
	{
		auto filePath = QStringToPath(dialog.selectedFiles().first());
		m_lastPath = filePath.parent_path();
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);

		if(m_virtualMachine != nullptr)
		{
			try
			{
				TryRegisteringBootable(filePath);
				m_lastOpenCommand = LastOpenCommand(BootType::CD, filePath);
				BootCDROM();
			}
			catch(const std::exception& e)
			{
				QMessageBox messageBox;
				messageBox.critical(nullptr, "Error", e.what());
				messageBox.show();
			}
		}
	}
}

void MainWindow::on_actionBoot_DiscImage_S3_triggered()
{
	S3FileBrowser browser(this);
	if(browser.exec())
	{
		auto filePath = browser.GetSelectedPath();
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);
		if(m_virtualMachine != nullptr)
		{
			try
			{
				BootCDROM();
			}
			catch(const std::exception& e)
			{
				QMessageBox messageBox;
				messageBox.critical(nullptr, "Error", e.what());
				messageBox.show();
			}
		}
	}
}

void MainWindow::on_actionBoot_cdrom0_triggered()
{
	try
	{
		BootCDROM();
	}
	catch(const std::exception& e)
	{
		QMessageBox messageBox;
		messageBox.critical(nullptr, "Error", e.what());
		messageBox.show();
	}
}

void MainWindow::on_actionBoot_ELF_triggered()
{
	QFileDialog dialog(this);
	dialog.setDirectory(PathToQString(m_lastPath));
	dialog.setFileMode(QFileDialog::ExistingFile);
	dialog.setNameFilter(tr("ELF files (*.elf)"));
	if(dialog.exec())
	{
		auto filePath = QStringToPath(dialog.selectedFiles().first());
		m_lastPath = filePath.parent_path();
		if(m_virtualMachine != nullptr)
		{
			try
			{
				BootElf(filePath);
			}
			catch(const std::exception& e)
			{
				QMessageBox messageBox;
				messageBox.critical(nullptr, "Error", e.what());
				messageBox.show();
			}
		}
	}
}

void MainWindow::BootElf(fs::path filePath)
{
	BootablesDb::CClient::GetInstance().SetLastBootedTime(filePath, std::time(nullptr));

	m_lastOpenCommand = LastOpenCommand(BootType::ELF, filePath);
	m_virtualMachine->Pause();
	m_virtualMachine->Reset();
	m_virtualMachine->m_ee->m_os->BootFromFile(filePath);
#ifndef DEBUGGER_INCLUDED
	m_virtualMachine->Resume();
#endif
	m_msgLabel->setText(QString("Loaded executable '%1'.")
	                        .arg(m_virtualMachine->m_ee->m_os->GetExecutableName()));
}

void MainWindow::LoadCDROM(fs::path filePath)
{
	m_lastPath = filePath.parent_path();
	CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);
}

void MainWindow::BootCDROM()
{
	auto filePath = CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_CDROM0_PATH);
	m_lastOpenCommand = LastOpenCommand(BootType::CD, filePath);
	BootablesDb::CClient::GetInstance().SetLastBootedTime(filePath, std::time(nullptr));
	m_virtualMachine->Pause();
	m_virtualMachine->Reset();
	m_virtualMachine->m_ee->m_os->BootFromCDROM();
#ifndef DEBUGGER_INCLUDED
	m_virtualMachine->Resume();
#endif
	m_msgLabel->setText(QString("Loaded executable '%1' from cdrom0.")
	                        .arg(m_virtualMachine->m_ee->m_os->GetExecutableName()));
}

void MainWindow::on_actionExit_triggered()
{
	close();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
	if((event->key() != Qt::Key_Escape) && m_qtKeyInputProvider)
	{
		m_qtKeyInputProvider->OnKeyPress(event->key());
	}
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
	if(event->key() == Qt::Key_Escape)
	{
		if(isFullScreen())
		{
			toggleFullscreen();
		}
		return;
	}
	if(m_qtKeyInputProvider)
	{
		m_qtKeyInputProvider->OnKeyRelease(event->key());
	}
}

void MainWindow::CreateStatusBar()
{
	m_fpsLabel = new QLabel("");
	m_fpsLabel->setAlignment(Qt::AlignHCenter);
	m_fpsLabel->setMinimumSize(m_fpsLabel->sizeHint());

	m_msgLabel = new ElidedLabel();
	m_msgLabel->setAlignment(Qt::AlignLeft);
	QFontMetrics fm(m_msgLabel->font());
	m_msgLabel->setMinimumSize(fm.boundingRect("...").size());
	m_msgLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	statusBar()->addWidget(m_msgLabel, 1);
	statusBar()->addWidget(m_fpsLabel);

	m_msgLabel->setText(QString("Play! v%1 - %2").arg(PLAY_VERSION).arg(__DATE__));

	m_fpsTimer = new QTimer(this);
	connect(m_fpsTimer, SIGNAL(timeout()), this, SLOT(updateStats()));
	m_fpsTimer->start(1000);
}

void MainWindow::updateStats()
{
	uint32 frames = CStatsManager::GetInstance().GetFrames();
	uint32 drawCalls = CStatsManager::GetInstance().GetDrawCalls();
	uint32 dcpf = (frames != 0) ? (drawCalls / frames) : 0;
#ifdef PROFILE
	m_profileStatsLabel->setText(QString::fromStdString(CStatsManager::GetInstance().GetProfilingInfo()));
#endif
	m_fpsLabel->setText(QString("%1 f/s, %2 dc/f").arg(frames).arg(dcpf));
	CStatsManager::GetInstance().ClearStats();
}

void MainWindow::on_actionSettings_triggered()
{
	SettingsDialog sd;
	sd.exec();
	SetupSoundHandler();
	if(m_virtualMachine != nullptr)
	{
		m_virtualMachine->ReloadSpuBlockCount();
		outputWindow_resized();
		auto gsHandler = m_virtualMachine->GetGSHandler();
		if(gsHandler)
		{
			gsHandler->NotifyPreferencesChanged();
		}
	}
}

void MainWindow::SetupSaveLoadStateSlots()
{
	bool enable = (m_virtualMachine != nullptr ? (m_virtualMachine->m_ee->m_os->GetELF() != nullptr) : false);
	ui->menuSave_States->clear();
	ui->menuLoad_States->clear();
	for(int i = 0; i < 10; i++)
	{
		QString info = enable ? GetSaveStateInfo(i) : "Empty";

		QAction* saveaction = new QAction(this);
		saveaction->setText(QString("Save Slot %1 - %2").arg(i).arg(info));
		saveaction->setEnabled(enable);
		ui->menuSave_States->addAction(saveaction);

		QAction* loadaction = new QAction(this);
		loadaction->setText(QString("Load Slot %1 - %2").arg(i).arg(info));
		loadaction->setEnabled(enable);
		ui->menuLoad_States->addAction(loadaction);

		if(enable)
		{
			connect(saveaction, &QAction::triggered, std::bind(&MainWindow::saveState, this, i));
			connect(loadaction, &QAction::triggered, std::bind(&MainWindow::loadState, this, i));
		}
	}
}

void MainWindow::saveState(int stateSlot)
{
	auto stateFilePath = m_virtualMachine->GenerateStatePath(stateSlot);
	auto future = m_virtualMachine->SaveState(stateFilePath);
	m_continuationChecker->GetContinuationManager().Register(std::move(future),
	                                                         [this, stateSlot = stateSlot](const bool& succeeded) {
		                                                         if(succeeded)
		                                                         {
			                                                         m_msgLabel->setText(QString("Saved state to slot %1.").arg(stateSlot));
			                                                         QString datetime = GetSaveStateInfo(stateSlot);
			                                                         ui->menuSave_States->actions().at(stateSlot)->setText(QString("Save Slot %1 - %2").arg(stateSlot).arg(datetime));
			                                                         ui->menuLoad_States->actions().at(stateSlot)->setText(QString("Load Slot %1 - %2").arg(stateSlot).arg(datetime));
		                                                         }
		                                                         else
		                                                         {
			                                                         m_msgLabel->setText(QString("Error saving state to slot %1.").arg(stateSlot));
		                                                         }
	                                                         });
}

void MainWindow::loadState(int stateSlot)
{
	auto stateFilePath = m_virtualMachine->GenerateStatePath(stateSlot);
	auto future = m_virtualMachine->LoadState(stateFilePath);
	m_continuationChecker->GetContinuationManager().Register(std::move(future),
	                                                         [this, stateSlot = stateSlot](const bool& succeeded) {
		                                                         if(succeeded)
		                                                         {
			                                                         m_msgLabel->setText(QString("Loaded state from slot %1.").arg(stateSlot));
#ifndef DEBUGGER_INCLUDED
			                                                         m_virtualMachine->Resume();
#endif
		                                                         }
		                                                         else
		                                                         {
			                                                         m_msgLabel->setText(QString("Error loading state from slot %1.").arg(stateSlot));
		                                                         }
	                                                         });
}

QString MainWindow::GetSaveStateInfo(int stateSlot)
{
	auto stateFilePath = m_virtualMachine->GenerateStatePath(stateSlot);
	QFileInfo file(PathToQString(stateFilePath));
	if(file.exists() && file.isFile())
	{
		return file.lastModified().toString("hh:mm dd.MM.yyyy");
	}
	else
	{
		return "Empty";
	}
}

void MainWindow::on_actionPause_Resume_triggered()
{
	if(m_virtualMachine != nullptr)
	{
		if(m_virtualMachine->GetStatus() == CVirtualMachine::PAUSED)
		{
			m_msgLabel->setText("Virtual machine resumed.");
			m_virtualMachine->Resume();
		}
		else
		{
			m_msgLabel->setText("Virtual machine paused.");
			m_virtualMachine->Pause();
		}
	}
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	QMessageBox::StandardButton resBtn = QMessageBox::question(this, "Close Confirmation?",
	                                                           tr("Are you sure you want to exit?\nHave you saved your progress?\n"),
	                                                           QMessageBox::Yes | QMessageBox::No,
	                                                           QMessageBox::Yes);
	if(resBtn != QMessageBox::Yes)
	{
		event->ignore();
	}
	else
	{
		event->accept();
	}
}

void MainWindow::on_actionAbout_triggered()
{
	auto about = QString("Version %1 (%2)\nQt v%3 - zlib v%4")
	                 .arg(QString(PLAY_VERSION), __DATE__, QT_VERSION_STR, ZLIB_VERSION);
	QMessageBox::about(this, this->windowTitle(), about);
}

void MainWindow::EmitOnExecutableChange()
{
	emit onExecutableChange();
}

void MainWindow::HandleOnExecutableChange()
{
	UpdateUI();
	auto titleString = QString("Play! - [ %1 ] - %2").arg(m_virtualMachine->m_ee->m_os->GetExecutableName(), QString(PLAY_VERSION));
	setWindowTitle(titleString);
}

void MainWindow::UpdateUI()
{
	ui->actionPause_when_focus_is_lost->setChecked(m_pauseFocusLost);
	ui->actionReset->setEnabled(!m_lastOpenCommand.path.empty());
	SetOutputWindowSize();
	SetupSaveLoadStateSlots();
}

void MainWindow::RegisterPreferences()
{
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREFERENCE_AUDIO_ENABLEOUTPUT, true);
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST, true);
}

void MainWindow::focusOutEvent(QFocusEvent* event)
{
	if(m_pauseFocusLost && m_virtualMachine->GetStatus() == CVirtualMachine::RUNNING)
	{
		if(!isActiveWindow() && !m_outputwindow->isActive())
		{
			if(m_virtualMachine != nullptr)
			{
				m_virtualMachine->Pause();
				m_deactivatePause = true;
			}
		}
	}
}
void MainWindow::focusInEvent(QFocusEvent* event)
{
	if(m_pauseFocusLost && m_virtualMachine->GetStatus() == CVirtualMachine::PAUSED)
	{
		if(m_deactivatePause && (isActiveWindow() || m_outputwindow->isActive()))
		{
			if(m_virtualMachine != nullptr)
			{
				m_virtualMachine->Resume();
				m_deactivatePause = false;
			}
		}
	}
}

void MainWindow::doubleClickEvent(QMouseEvent* ev)
{
	if(ev->button() == Qt::LeftButton)
	{
		toggleFullscreen();
	}
}

void MainWindow::toggleFullscreen()
{
	if(isFullScreen())
	{
		showNormal();
		statusBar()->show();
		menuBar()->show();
	}
	else
	{
		showFullScreen();
		statusBar()->hide();
		menuBar()->hide();
	}
}

#ifdef DEBUGGER_INCLUDED
bool MainWindow::nativeEventFilter(const QByteArray&, void* message, long* result)
{
	auto msg = reinterpret_cast<MSG*>(message);
	HWND activeWnd = GetActiveWindow();
	if(m_debugger && (activeWnd == m_debugger->m_hWnd) &&
	   TranslateAccelerator(m_debugger->m_hWnd, m_debugger->GetAccelerators(), msg))
	{
		return true;
	}
	if(m_frameDebugger && (activeWnd == m_frameDebugger->m_hWnd) &&
	   TranslateAccelerator(m_frameDebugger->m_hWnd, m_frameDebugger->GetAccelerators(), msg))
	{
		return true;
	}
	return false;
}

void MainWindow::ShowDebugger()
{
	m_debugger->Show(SW_SHOWMAXIMIZED);
	SetForegroundWindow(*m_debugger);
}

void MainWindow::ShowFrameDebugger()
{
	m_frameDebugger->Show(SW_SHOWMAXIMIZED);
	SetForegroundWindow(*m_frameDebugger);
}

fs::path MainWindow::GetFrameDumpDirectoryPath()
{
	return CAppConfig::GetBasePath() / fs::path("framedumps/");
}

void MainWindow::DumpNextFrame()
{
	m_virtualMachine->TriggerFrameDump(
	    [&](const CFrameDump& frameDump) {
		    try
		    {
			    auto frameDumpDirectoryPath = GetFrameDumpDirectoryPath();
			    Framework::PathUtils::EnsurePathExists(frameDumpDirectoryPath);
			    for(unsigned int i = 0; i < UINT_MAX; i++)
			    {
				    auto frameDumpFileName = string_format("framedump_%08d.dmp.zip", i);
				    auto frameDumpPath = frameDumpDirectoryPath / fs::path(frameDumpFileName);
				    if(!fs::exists(frameDumpPath))
				    {
					    auto dumpStream = Framework::CreateOutputStdStream(frameDumpPath.native());
					    frameDump.Write(dumpStream);
					    m_msgLabel->setText(QString("Dumped frame to '%1'.").arg(frameDumpFileName.c_str()));
					    return;
				    }
			    }
		    }
		    catch(...)
		    {
		    }
		    m_msgLabel->setText(QString("Failed to dump frame."));
	    });
}

void MainWindow::ToggleGsDraw()
{
	auto gs = m_virtualMachine->GetGSHandler();
	if(gs == nullptr) return;
	bool newState = !gs->GetDrawEnabled();
	gs->SetDrawEnabled(newState);
	debugMenuUi->actionGsDrawEnabled->setChecked(newState);
	m_msgLabel->setText(newState ? QString("GS Draw Enabled") : QString("GS Draw Disabled"));
}

#endif

void MainWindow::on_actionPause_when_focus_is_lost_triggered(bool checked)
{
	m_pauseFocusLost = checked;
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST, m_pauseFocusLost);
}

void MainWindow::on_actionReset_triggered()
{
	if(!m_lastOpenCommand.path.empty())
	{
		if(m_lastOpenCommand.type == BootType::CD)
		{
			BootCDROM();
		}
		else if(m_lastOpenCommand.type == BootType::ELF)
		{
			BootElf(m_lastOpenCommand.path);
		}
	}
}

void MainWindow::on_actionMemory_Card_Manager_triggered()
{
	MemoryCardManagerDialog mcm(this);
	mcm.exec();
}

void MainWindow::on_actionVFS_Manager_triggered()
{
	VFSManagerDialog vfsmgr;
	vfsmgr.exec();
}

void MainWindow::on_actionController_Manager_triggered()
{
	auto padHandler = static_cast<CPH_GenericInput*>(m_virtualMachine->GetPadHandler());
	if(!padHandler) return;

	ControllerConfigDialog ccd(&padHandler->GetBindingManager(), m_qtKeyInputProvider.get(), this);
	ccd.exec();
}

void MainWindow::on_actionCapture_Screen_triggered()
{
	m_screenShotCompleteConnection = CScreenShotUtils::TriggerGetScreenshot(m_virtualMachine,
	                                                                        [&](int res, const char* msg) -> void {
		                                                                        m_msgLabel->setText(msg);
	                                                                        });
}

void MainWindow::on_actionList_Bootables_triggered()
{
	BootableListDialog dialog(this);
	if(dialog.exec())
	{
		try
		{
			BootablesDb::Bootable bootable = dialog.getResult();
			if(IsBootableDiscImagePath(bootable.path))
			{
				LoadCDROM(bootable.path);
				BootCDROM();
			}
			else if(IsBootableExecutablePath(bootable.path))
			{
				BootElf(bootable.path);
			}
			else
			{
				QMessageBox messageBox;
				QString invalid("Invalid File Format.");
				messageBox.critical(this, this->windowTitle(), invalid);
				messageBox.show();
			}
		}
		catch(const std::exception& e)
		{
			QMessageBox messageBox;
			messageBox.critical(nullptr, "Error", e.what());
			messageBox.show();
		}
	}
}
