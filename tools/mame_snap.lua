local n=0
emu.register_frame_done(function()
  n=n+1
  if n==180 then
    manager.machine.video:snapshot()
  elseif n>=185 then
    manager.machine:exit()
  end
end)
