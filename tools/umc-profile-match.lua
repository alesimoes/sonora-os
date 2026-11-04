-- Run by tools/test-umc-profile.sh through wpexec. Feeds fake node properties
-- through the call the ALSA monitor makes (scripts/monitors/alsa.lua,
-- WirePlumber 0.5.8: JsonUtils.match_rules_update_properties (rules, props))
-- against the shipped monitor.alsa.rules, and prints one MATCH line per node.
local rules = Conf.get_section_as_json ("monitor.alsa.rules", Json.Array {})

local nodes = {
  "alsa_input.usb-BEHRINGER_UMC1820_1234567-00.pro-input-0",
  "alsa_output.usb-BEHRINGER_UMC1820_1234567-00.pro-output-0",
  "alsa_input.pci-0000_00_1f.3.pro-input-0",
  "alsa_input.usb-BEHRINGER_UMC202HD_1-00.pro-input-0",
}

for _, name in ipairs (nodes) do
  local props = JsonUtils.match_rules_update_properties (rules, { ["node.name"] = name })
  print (string.format ("MATCH %s suspend=%s period=%s", name,
      tostring (props["session.suspend-timeout-seconds"]),
      tostring (props["api.alsa.period-size"])))
end
Core.quit ()
