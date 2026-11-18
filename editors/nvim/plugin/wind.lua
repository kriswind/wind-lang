-- vim.lsp.enable exists from Neovim 0.11; older versions need an explicit
-- vim.lsp.start() call (see editors/README.md).
if vim.lsp.enable then
  vim.lsp.enable("wind")
end

-- Running a program: ":WindRun [file]" runs the current file, or the one
-- named, with "wind run" in a terminal along the bottom of the screen, and
-- ":WindStop" stops it, leaving what it printed in view. There is one such
-- terminal: running again stops the program it holds, which for a program
-- on a board also frees the board's serial port, and runs the new one in
-- its place.
--
-- The keyboard stays where it was, as in other editors, and the terminal
-- follows what the program prints while the cursor is elsewhere; with the
-- cursor in it, it stays where it is, so that earlier output can be read.
-- With vim.g.wind_run_focus set to "terminal", the keyboard goes to the
-- terminal instead. ":q" in the terminal closes it and stops the program.
local run = { buf = nil, job = nil, file = nil }

-- Stops the program, if one is running, and leaves the terminal as it is.
local function stop_program()
  if run.job then
    pcall(vim.fn.jobstop, run.job)
    run.job = nil
  end
end

local function close()
  stop_program()
  if run.buf and vim.api.nvim_buf_is_valid(run.buf) then
    vim.api.nvim_buf_delete(run.buf, { force = true })
  end
  run.buf = nil
end

-- Shows the end of the terminal in every window of it but the one the
-- cursor is in.
local function follow(buf)
  if not vim.api.nvim_buf_is_valid(buf) then
    return
  end
  local last = vim.api.nvim_buf_line_count(buf)
  for _, win in ipairs(vim.fn.win_findbuf(buf)) do
    if win ~= vim.api.nvim_get_current_win() then
      vim.api.nvim_win_set_cursor(win, { last, 0 })
    end
  end
end

local function start(file)
  close()
  run.file = file
  local from = vim.api.nvim_get_current_win()
  local height = math.max(8, math.floor(vim.o.lines * 0.3))
  vim.cmd("botright " .. height .. "split")
  local buf = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_win_set_buf(0, buf)
  run.buf = buf
  local cmd = { "wind", "run", file }
  local options = {
    on_exit = function(job)
      if run.job == job then
        run.job = nil
      end
    end,
  }
  if vim.fn.has("nvim-0.11") == 1 then
    options.term = true
    run.job = vim.fn.jobstart(cmd, options)
  else
    run.job = vim.fn.termopen(cmd, options)
  end
  -- Closing the window stops the program, so that nothing goes on running
  -- unseen, holding a board's serial port.
  vim.bo[buf].bufhidden = "wipe"
  vim.api.nvim_buf_attach(buf, false, {
    on_lines = function()
      vim.schedule(function()
        follow(buf)
      end)
    end,
  })
  for _, key in ipairs({ "<F5>", "<S-F5>", "<F17>" }) do
    local action = key == "<F5>" and function()
      start(run.file)
    end or stop_program
    vim.keymap.set({ "n", "t" }, key, action, { buffer = buf, desc = "Run or stop the Wind program" })
  end
  if vim.g.wind_run_focus == "terminal" then
    vim.cmd.startinsert()
  elseif vim.api.nvim_win_is_valid(from) then
    vim.api.nvim_set_current_win(from)
  end
end

vim.api.nvim_create_user_command("WindRun", function(opts)
  local file = opts.args
  if file == "" then
    if run.buf and vim.api.nvim_get_current_buf() == run.buf then
      file = run.file
    else
      file = vim.api.nvim_buf_get_name(0)
      if file == "" then
        vim.notify("WindRun: this buffer has no file to run", vim.log.levels.ERROR)
        return
      end
      -- The program is built from what is on disk.
      vim.cmd("silent update")
    end
  end
  start(vim.fn.fnamemodify(file, ":p"))
end, { nargs = "?", complete = "file", desc = "Run a Wind program in a terminal below" })

vim.api.nvim_create_user_command("WindStop", stop_program, { desc = "Stop the running Wind program" })
