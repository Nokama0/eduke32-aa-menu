#pragma once

// Mutates engine globals: call only from the SwapBuffers hook, never the scan worker.
bool cansee_scan(void);
bool cansee_ready(void);

unsigned cansee_rva(void);

bool ek_cansee(int x1, int y1, int z1, int sect1,
               int x2, int y2, int z2, int sect2);
