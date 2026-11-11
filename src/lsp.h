#ifndef WIND_LSP_H
#define WIND_LSP_H

/* Serves the Language Server Protocol on stdin/stdout until the client sends
   "exit" or closes the stream. Returns the process exit code. */
int lsp_run(void);

#endif
