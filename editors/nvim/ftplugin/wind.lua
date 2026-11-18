vim.bo.commentstring = "// %s"
vim.bo.comments = "s1:/*,mb:*,ex:*/,://"

-- F5 saves the file and runs it in a terminal below, and Shift-F5 stops it;
-- see :WindRun and :WindStop. A terminal may send Shift-F5 as F17.
vim.keymap.set({ "n", "i" }, "<F5>", "<Cmd>WindRun<CR>", { buffer = true, desc = "Run this Wind program" })
for _, key in ipairs({ "<S-F5>", "<F17>" }) do
  vim.keymap.set({ "n", "i" }, key, "<Cmd>WindStop<CR>", { buffer = true, desc = "Stop the Wind program" })
end
