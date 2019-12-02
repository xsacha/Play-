#pragma once

#include <QtGui/QWindow>

class OutputWindow : public QWindow
{
	Q_OBJECT
public:
	explicit OutputWindow(QWindow* parent = 0);
	~OutputWindow() = default;

protected:
	void exposeEvent(QExposeEvent* ev) Q_DECL_OVERRIDE;
	void keyPressEvent(QKeyEvent*) Q_DECL_OVERRIDE;
	void keyReleaseEvent(QKeyEvent*) Q_DECL_OVERRIDE;
	void focusOutEvent(QFocusEvent*) Q_DECL_OVERRIDE;
	void focusInEvent(QFocusEvent*) Q_DECL_OVERRIDE;
	void mouseDoubleClickEvent(QMouseEvent*) Q_DECL_OVERRIDE;

signals:
	void keyDown(QKeyEvent*);
	void keyUp(QKeyEvent*);
	void focusOut(QFocusEvent*);
	void focusIn(QFocusEvent*);
	void doubleClick(QMouseEvent*);

private slots:
	void activeStateChanged();
};
