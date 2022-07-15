
//
// For STF tracing a known hot loop, use START/STOP_TRACE macros to
// enable/disable trace generation
//

// old-school guards
#ifndef TRACE_MARKERS_H
#define TRACE_MARKERS_H

#ifndef START_TRACE
#define START_TRACE  asm("xor x0, x0, x0");
#endif

#ifndef START_TRACE_OPC
#define START_TRACE_OPC 0x00004033
#endif

#ifndef STOP_TRACE
#define STOP_TRACE   asm("xor x0, x1, x1");
#endif

#ifndef STOP_TRACE_OPC
#define STOP_TRACE_OPC 0x00010c033
#endif

#endif
