"""Qt 绑定兼容层：优先 PySide6，失败回退 PyQt5。

本机 anaconda 环境的 PySide6 QtWidgets DLL 加载失败，PyQt5 正常；
两者 API 差异集中在这里统一抹平。
"""
from __future__ import annotations

try:
    from PySide6.QtCore import (QAbstractTableModel, QModelIndex, QObject, Qt,
                                QRunnable, QThreadPool, QTimer, Signal, QEvent)
    from PySide6.QtGui import (QAction, QColor, QImage, QPainter, QPixmap,
                               QCursor)
    from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog,
                                   QDialogButtonBox, QFileDialog, QHBoxLayout,
                                   QHeaderView, QLabel, QLineEdit,
                                   QMainWindow, QMessageBox, QPlainTextEdit,
                                   QProgressDialog, QPushButton, QScrollArea,
                                   QSlider, QSpinBox,
                                   QSplitter, QStackedWidget, QStatusBar,
                                   QTableView, QVBoxLayout, QWidget)
    QT_BIND = "PySide6"
    FMT_RGBA8888 = QImage.Format.Format_RGBA8888
except ImportError:  # PyQt5
    from PyQt5.QtCore import (QAbstractTableModel, QModelIndex, QObject, Qt,
                              QRunnable, QThreadPool, QTimer, QEvent)
    from PyQt5.QtCore import pyqtSignal as Signal
    from PyQt5.QtGui import QColor, QImage, QPainter, QPixmap, QCursor
    from PyQt5.QtWidgets import (QAction, QApplication, QCheckBox, QComboBox,
                                 QDialog, QDialogButtonBox, QFileDialog,
                                 QHBoxLayout, QHeaderView, QLabel, QLineEdit,
                                 QMainWindow, QMessageBox, QPlainTextEdit,
                                 QProgressDialog, QPushButton, QScrollArea,
                                 QSlider, QSpinBox,
                                 QSplitter, QStackedWidget, QStatusBar,
                                 QTableView, QVBoxLayout, QWidget)
    QT_BIND = "PyQt5"
    FMT_RGBA8888 = QImage.Format_RGBA8888


def app_exec(app) -> int:
    return app.exec() if hasattr(app, "exec") else app.exec_()
