#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QKeyEvent>
#include <QDir>
#include <QAbstractNativeEventFilter>

#include "AppConfig.h"
#include "PS2VM.h"
#include "ElidedLabel.h"
#include "ContinuationChecker.h"

#include "InputProviderQtKey.h"
#include "ScreenShotUtils.h"

namespace Ui
{
	class MainWindow;
}

#ifdef DEBUGGER_INCLUDED
class CDebugger;
class CFrameDebugger;

namespace Ui
{
	class DebugMenu;
}
#endif

class MainWindow : public QMainWindow
#ifdef DEBUGGER_INCLUDED
    ,
                   public QAbstractNativeEventFilter
#endif
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

	void BootElf(fs::path);
	void BootCDROM();
	void LoadCDROM(fs::path filePath);
	void loadState(int);

#ifdef DEBUGGER_INCLUDED
	void ShowDebugger();
	void ShowFrameDebugger();
	fs::path GetFrameDumpDirectoryPath();
	void DumpNextFrame();
	void ToggleGsDraw();
#endif

private:
	enum BootType
	{
		CD,
		ELF
	};

	struct LastOpenCommand
	{
		LastOpenCommand() = default;
		LastOpenCommand(BootType type, fs::path path)
		    : type(type)
		    , path(path)
		{
		}
		BootType type = BootType::CD;
		fs::path path;
	};

	void SetOutputWindowSize();
	void CreateStatusBar();
	void InitVirtualMachine();
	void SetupGsHandler();
	void SetupSoundHandler();
	void SetupSaveLoadStateSlots();
	QString GetSaveStateInfo(int);
	void EmitOnExecutableChange();
	void UpdateUI();
	void RegisterPreferences();
	void saveState(int);
	void toggleFullscreen();
#ifdef DEBUGGER_INCLUDED
	bool nativeEventFilter(const QByteArray&, void*, long*) Q_DECL_OVERRIDE;
#endif

	Ui::MainWindow* ui;

	QWindow* m_outputwindow = nullptr;
	QLabel* m_fpsLabel = nullptr;
#ifdef PROFILE
	QLabel* m_profileStatsLabel = nullptr;
	CPS2VM::ProfileFrameDoneSignal::Connection m_profileFrameDoneConnection;
#endif
	ElidedLabel* m_msgLabel = nullptr;
	QTimer* m_fpsTimer = nullptr;
	CContinuationChecker* m_continuationChecker = nullptr;
	CPS2VM* m_virtualMachine = nullptr;
	bool m_deactivatePause = false;
	bool m_pauseFocusLost = true;
	std::shared_ptr<CInputProviderQtKey> m_qtKeyInputProvider;
	LastOpenCommand m_lastOpenCommand;
	fs::path m_lastPath;

	Framework::CSignal<void()>::Connection m_OnExecutableChangeConnection;
	CGSHandler::NewFrameEvent::Connection m_OnNewFrameConnection;
	CScreenShotUtils::Connection m_screenShotCompleteConnection;

#ifdef DEBUGGER_INCLUDED
	std::unique_ptr<CDebugger> m_debugger;
	std::unique_ptr<CFrameDebugger> m_frameDebugger;
	Ui::DebugMenu* debugMenuUi = nullptr;
#endif

protected:
	void closeEvent(QCloseEvent*) Q_DECL_OVERRIDE;
	void showEvent(QShowEvent*) Q_DECL_OVERRIDE;

signals:
	void onExecutableChange();

public slots:
	void outputWindow_resized();
	void updateStats();

private slots:
	void on_actionBoot_DiscImage_triggered();
	void on_actionBoot_DiscImage_S3_triggered();
	void on_actionBoot_cdrom0_triggered();
	void on_actionBoot_ELF_triggered();
	void on_actionExit_triggered();
	void keyPressEvent(QKeyEvent*) Q_DECL_OVERRIDE;
	void keyReleaseEvent(QKeyEvent*) Q_DECL_OVERRIDE;
	void on_actionSettings_triggered();
	void on_actionPause_Resume_triggered();
	void on_actionAbout_triggered();
	void focusOutEvent(QFocusEvent*) Q_DECL_OVERRIDE;
	void focusInEvent(QFocusEvent*) Q_DECL_OVERRIDE;
	void on_actionPause_when_focus_is_lost_triggered(bool checked);
	void on_actionReset_triggered();
	void on_actionMemory_Card_Manager_triggered();
	void on_actionVFS_Manager_triggered();
	void on_actionController_Manager_triggered();
	void on_actionCapture_Screen_triggered();
	void doubleClickEvent(QMouseEvent*);
	void HandleOnExecutableChange();
	void on_actionList_Bootables_triggered();
};

#endif // MAINWINDOW_H
