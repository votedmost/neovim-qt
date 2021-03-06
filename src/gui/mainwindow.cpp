#include "mainwindow.h"

#include <QCloseEvent>
#include <QLayout>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolBar>

namespace NeovimQt {

MainWindow::MainWindow(NeovimConnector *c, ShellOptions opts, QWidget *parent)
:QMainWindow(parent), m_nvim(0), m_errorWidget(0), m_shell(0),
	m_delayedShow(DelayedShow::Disabled), m_tabline(0), m_tabline_bar(0),
	m_shell_options(opts), m_neovim_requested_close(false)
{
	m_errorWidget = new ErrorWidget();
	m_stack.addWidget(m_errorWidget);
	connect(m_errorWidget, &ErrorWidget::reconnectNeovim,
			this, &MainWindow::reconnectNeovim);
	setCentralWidget(&m_stack);

	init(c);
}

void MainWindow::init(NeovimConnector *c)
{
	if (m_shell) {
		m_shell->deleteLater();
		m_stack.removeWidget(m_shell);
	}
	if (m_nvim) {
		m_nvim->deleteLater();
	}

	m_tabline_bar = addToolBar("tabline");
	m_tabline_bar->setObjectName("tabline");
	m_tabline_bar->setAllowedAreas(Qt::TopToolBarArea);
	m_tabline_bar->setMovable(false);
	m_tabline_bar->setFloatable(false);
	// Avoid margins around the tabbar
	m_tabline_bar->layout()->setContentsMargins(0, 0, 0, 0);

	m_tabline = new QTabBar(m_tabline_bar);
	m_tabline->setDrawBase(false);
	m_tabline->setExpanding(false);
	m_tabline->setDocumentMode(true);
	m_tabline->setFocusPolicy(Qt::NoFocus);
	connect(m_tabline, &QTabBar::currentChanged,
			this, &MainWindow::changeTab);

	m_tabline_bar->addWidget(m_tabline);
	m_tabline_bar->setVisible(m_shell_options.enable_ext_tabline);

	// Context menu and actions for right-click
	m_contextMenu = new QMenu();
	m_actCut = new QAction(QIcon::fromTheme("edit-cut"), QString("Cut"), nullptr /*parent*/);
	m_actCopy = new QAction(QIcon::fromTheme("edit-copy"), QString("Copy"), nullptr /*parent*/);
	m_actPaste = new QAction(QIcon::fromTheme("edit-paste"), QString("Paste"), nullptr /*parent*/);
	m_actSelectAll = new QAction(QIcon::fromTheme("edit-select-all"), QString("Select All"),
		nullptr /*parent*/);
	m_contextMenu->addAction(m_actCut);
	m_contextMenu->addAction(m_actCopy);
	m_contextMenu->addAction(m_actPaste);
	m_contextMenu->addSeparator();
	m_contextMenu->addAction(m_actSelectAll);

	m_nvim = c;

	m_tree = new TreeView(c);
	m_shell = new Shell(c, m_shell_options);

	// GuiScrollBar
	m_rightScrollBar = new QScrollBar{};
	m_rightScrollBar->setVisible(false);
	m_rightScrollBar->setMinimum(1);

	// ShellWidget + GuiScrollBar Layout
	// QSplitter does not allow layouts directly: QWidget { HLayout { ShellWidget, QScrollBar } }
	QWidget* shellScrollable{ new QWidget() };
	QHBoxLayout* layout{ new QHBoxLayout() };
	layout->setSpacing(0);
	layout->setMargin(0);
	layout->addWidget(m_shell);
	layout->addWidget(m_rightScrollBar);
	shellScrollable->setLayout(layout);

	m_window = new QSplitter();
	m_window->addWidget(m_tree);
	m_tree->hide();
	m_window->addWidget(shellScrollable);

	m_stack.insertWidget(1, m_window);
	m_stack.setCurrentIndex(1);

	connect(m_shell, SIGNAL(neovimAttached(bool)),
			this, SLOT(neovimAttachmentChanged(bool)));
	connect(m_shell, SIGNAL(neovimTitleChanged(const QString &)),
			this, SLOT(neovimSetTitle(const QString &)));
	connect(m_shell, &Shell::neovimResized,
			this, &MainWindow::neovimWidgetResized);
	connect(m_shell, &Shell::neovimMaximized,
			this, &MainWindow::neovimMaximized);
	connect(m_shell, &Shell::neovimSuspend,
			this, &MainWindow::neovimSuspend);
	connect(m_shell, &Shell::neovimFullScreen,
			this, &MainWindow::neovimFullScreen);
	connect(m_shell, &Shell::neovimGuiCloseRequest,
			this, &MainWindow::neovimGuiCloseRequest);
	connect(m_nvim, &NeovimConnector::processExited,
			this, &MainWindow::neovimExited);
	connect(m_nvim, &NeovimConnector::error,
			this, &MainWindow::neovimError);
	connect(m_shell, &Shell::neovimIsUnsupported,
			this, &MainWindow::neovimIsUnsupported);
	connect(m_shell, &Shell::neovimExtTablineSet,
			this, &MainWindow::extTablineSet);
	connect(m_shell, &Shell::neovimTablineUpdate,
			this, &MainWindow::neovimTablineUpdate);
	connect(m_shell, &Shell::neovimShowtablineSet,
			this, &MainWindow::neovimShowtablineSet);
	connect(m_shell, &Shell::neovimShowContextMenu,
			this, &MainWindow::neovimShowContextMenu);
	connect(m_actCut, &QAction::triggered,
			this, &MainWindow::neovimSendCut);
	connect(m_actCopy, &QAction::triggered,
			this, &MainWindow::neovimSendCopy);
	connect(m_actPaste, &QAction::triggered,
			this, &MainWindow::neovimSendPaste);
	connect(m_actSelectAll, &QAction::triggered,
			this, &MainWindow::neovimSendSelectAll);

	// GuiScrollBar Signal/Slot Connections
	connect(m_shell, &Shell::setGuiScrollBarVisible,
			this, &MainWindow::setGuiScrollBarVisible);
	connect(m_shell, &Shell::neovimCursorMovedUpdateScrollBar,
			this, &MainWindow::neovimCursorMovedUpdateScrollBar);
	connect(m_shell, &Shell::neovimScrollEvent,
			this, &MainWindow::neovimScrollEvent);
	connect(m_rightScrollBar, &QScrollBar::valueChanged,
			m_shell, &Shell::handleScrollBarChanged);

	m_shell->setFocus(Qt::OtherFocusReason);

	if (m_nvim->errorCause()) {
		neovimError(m_nvim->errorCause());
	}
}

bool MainWindow::neovimAttached() const
{
	return (m_shell != NULL && m_shell->neovimAttached());
}

/** The Neovim process has exited */
void MainWindow::neovimExited(int status)
{
	showIfDelayed();

	if (m_nvim->errorCause() != NeovimConnector::NoError) {
		m_errorWidget->setText(m_nvim->errorString());
		m_errorWidget->showReconnect(m_nvim->canReconnect());
		m_stack.setCurrentIndex(0);
	} else if (status != 0) {
		m_errorWidget->setText(QString("Neovim exited with status code (%1)").arg(status));
		m_errorWidget->showReconnect(m_nvim->canReconnect());
		m_stack.setCurrentIndex(0);
	} else {
		close();
	}
}
void MainWindow::neovimError(NeovimConnector::NeovimError err)
{
	showIfDelayed();

	switch(err) {
	case NeovimConnector::FailedToStart:
		m_errorWidget->setText("Unable to start nvim: " + m_nvim->errorString());
		break;
	default:
		m_errorWidget->setText(m_nvim->errorString());
	}
	m_errorWidget->showReconnect(m_nvim->canReconnect());
	m_stack.setCurrentIndex(0);
}
void MainWindow::neovimIsUnsupported()
{
	showIfDelayed();
	m_errorWidget->setText(QString("Cannot connect to this Neovim, required API version 1, found [%1-%2]")
			.arg(m_nvim->apiCompatibility())
			.arg(m_nvim->apiLevel()));
	m_errorWidget->showReconnect(m_nvim->canReconnect());
	m_stack.setCurrentIndex(0);
}

void MainWindow::neovimSetTitle(const QString &title)
{
	this->setWindowTitle(title);
}

void MainWindow::neovimWidgetResized()
{
	m_shell->resizeNeovim(m_shell->size());
}

void MainWindow::neovimMaximized(bool set)
{
	if (set) {
		setWindowState(windowState() | Qt::WindowMaximized);
	} else {
		setWindowState(windowState() & ~Qt::WindowMaximized);
	}
}

void MainWindow::neovimSuspend()
{
	qDebug() << "Minimizing window";
	setWindowState(windowState() | Qt::WindowMinimized);
}

void MainWindow::neovimFullScreen(bool set)
{
	if (set) {
		setWindowState(windowState() | Qt::WindowFullScreen);
	} else {
		setWindowState(windowState() & ~Qt::WindowFullScreen);
	}
}

void MainWindow::neovimGuiCloseRequest()
{
	m_neovim_requested_close = true;
	QMainWindow::close();
	m_neovim_requested_close = false;
}

void MainWindow::reconnectNeovim()
{
	if (m_nvim->canReconnect()) {
		init(m_nvim->reconnect());
	}
	m_stack.setCurrentIndex(1);
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
	// Do not save window geometry in '--fullscreen' mode. If saved, all
	// subsequent Neovim-Qt sessions would default to fullscreen mode.
	if (!isFullScreen()) {
		saveWindowGeometry();
	}

	if (m_neovim_requested_close) {
		// If this was requested by nvim, shutdown
		QWidget::closeEvent(ev);
	} else if (m_shell->close()) {
		// otherwise only if the Neovim shell closes too
		QWidget::closeEvent(ev);
	} else {
		ev->ignore();
	}
}
void MainWindow::changeEvent( QEvent *ev)
{
	if (ev->type() == QEvent::WindowStateChange && isWindow()) {
		m_shell->updateGuiWindowState(windowState());
	}
	QWidget::changeEvent(ev);
}

/// Call show() after a 1s delay or when Neovim attachment
/// is complete, whichever comes first
void MainWindow::delayedShow(DelayedShow type)
{
	m_delayedShow = type;
	if (m_nvim->errorCause() != NeovimConnector::NoError) {
		showIfDelayed();
		return;
	}

	if (type != DelayedShow::Disabled) {
		QTimer *t = new QTimer(this);
		t->setSingleShot(true);
		t->setInterval(1000);
		connect(m_shell, &Shell::neovimResized, this, &MainWindow::showIfDelayed);
		connect(t, &QTimer::timeout, this, &MainWindow::showIfDelayed);
		t->start();
	}
}

void MainWindow::showIfDelayed()
{
	if (!isVisible()) {
		if (m_delayedShow == DelayedShow::Normal) {
			show();
		} else if (m_delayedShow == DelayedShow::Maximized) {
			showMaximized();
		} else if (m_delayedShow == DelayedShow::FullScreen) {
			showFullScreen();
		}
	}
	m_delayedShow = DelayedShow::Disabled;
}

void MainWindow::neovimAttachmentChanged(bool attached)
{
	emit neovimAttached(attached);
	if (attached) {
		if (isWindow() && m_shell != NULL) {
			m_shell->updateGuiWindowState(windowState());
		}
	} else {
		m_tabline->deleteLater();
		m_tabline_bar->deleteLater();
	}
}

Shell* MainWindow::shell()
{
	return m_shell;
}

void MainWindow::extTablineSet(bool val)
{
	bool old = m_shell_options.enable_ext_tabline;
	m_shell_options.enable_ext_tabline = val;
	// redraw if state changed
	if (old != m_shell_options.enable_ext_tabline) {
		if (!val) m_tabline_bar->setVisible(false);
		m_nvim->api0()->vim_command("silent! redraw!");
	}
}

void MainWindow::neovimShowtablineSet(int val)
{
	m_shell_options.nvim_show_tabline = val;
}

void MainWindow::neovimTablineUpdate(int64_t curtab, QList<Tab> tabs)
{
	if (!m_shell_options.enable_ext_tabline) {
		return;
	}

	// remove extra tabs
	for (int index=tabs.size(); index<m_tabline->count(); index++) {
		m_tabline->removeTab(index);
	}

	for (int index=0; index<tabs.size(); index++) {
		// Escape & in tab name otherwise it will be interpreted as
		// a keyboard shortcut (#357) - escaping is done using &&
		QString text = tabs[index].name;
		text.replace("&", "&&");

		if (m_tabline->count() <= index) {
			m_tabline->addTab(text);
		} else {
			m_tabline->setTabText(index, text);
		}

		m_tabline->setTabToolTip(index, text);
		m_tabline->setTabData(index, QVariant::fromValue(tabs[index].tab));

		if (curtab == tabs[index].tab) {
			m_tabline->setCurrentIndex(index);
		}
	}

	// hide/show the tabline toolbar
	if (m_shell_options.nvim_show_tabline==0) {
		m_tabline_bar->setVisible(false);
	} else if (m_shell_options.nvim_show_tabline==2) {
		m_tabline_bar->setVisible(true);
	} else {
		m_tabline_bar->setVisible(tabs.size() > 1);
	}

	Q_ASSERT(tabs.size() == m_tabline->count());
}

void MainWindow::neovimShowContextMenu()
{
	m_contextMenu->popup(QCursor::pos());
}

void MainWindow::setGuiScrollBarVisible(bool isEnabled)
{
	m_rightScrollBar->setVisible(isEnabled);
}

void MainWindow::neovimCursorMovedUpdateScrollBar(uint64_t minLineVisible, uint64_t bufferSize, uint64_t windowHeight)
{
	m_rightScrollBar->setMaximum(bufferSize);
	m_rightScrollBar->setPageStep(windowHeight);

	m_scrollbarLastDelta = minLineVisible - m_rightScrollBar->sliderPosition();

	// Block valueChanged signals for scroll events triggered by Neovim.
	// Do not be echoed back scroll events, only GUI events should be sent.
	{
		QSignalBlocker blockValueChanged{ m_rightScrollBar };
		m_rightScrollBar->setSliderPosition(minLineVisible);
	}
}

void MainWindow::neovimScrollEvent(int64_t rows)
{
	// Prevent double-registration of scroll event. See m_scrollbarLastDelta for details.
	if (rows == m_scrollbarLastDelta)
	{
		return;
	}
	m_scrollbarLastDelta = 0;

	// Block valueChanged signals for scroll events triggered by Neovim.
	// Do not be echoed back scroll events, only GUI events should be sent.
	{
		QSignalBlocker blockValueChanged{ m_rightScrollBar };
		m_rightScrollBar->setSliderPosition(m_rightScrollBar->sliderPosition() + rows);
	}
}

void MainWindow::neovimSendCut()
{
	m_nvim->api0()->vim_command_output(R"(normal! "+x)");
}

void MainWindow::neovimSendCopy()
{
	m_nvim->api0()->vim_command(R"(normal! "+y)");
}

void MainWindow::neovimSendPaste()
{
	m_nvim->api0()->vim_command(R"(normal! "+gP)");
}

void MainWindow::neovimSendSelectAll()
{
	m_nvim->api0()->vim_command("normal! ggVG");
}

void MainWindow::changeTab(int index)
{
	if (!m_shell_options.enable_ext_tabline) {
		return;
	}

	if (m_nvim->api2() == NULL) {
		return;
	}

	int64_t tab = m_tabline->tabData(index).toInt();
	m_nvim->api2()->nvim_set_current_tabpage(tab);
}

void MainWindow::saveWindowGeometry()
{
	QSettings settings{ "nvim-qt", "window-geometry" };
	settings.setValue("window_geometry", saveGeometry());
	settings.setValue("window_state", saveState());
}

void MainWindow::restoreWindowGeometry()
{
	QSettings settings{ "nvim-qt", "window-geometry" };
	restoreGeometry(settings.value("window_geometry").toByteArray());
	restoreState(settings.value("window_state").toByteArray());
}

}  // namespace NeovimQt
