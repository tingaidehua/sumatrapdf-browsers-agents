/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

TempStr BuildDevInfoTextTemp(MainWindow* win);
void ShowDevInfoDialog(MainWindow* win);
// Live HTTP probe of CDP /json/version on 127.0.0.1:port.
bool ProbeCdpPortAlive(int port, int* elapsedMsOut = nullptr, Str* errorOut = nullptr);
