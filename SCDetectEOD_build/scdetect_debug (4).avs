LoadPlugin("SCDetectEOD.dll")

src = LWLibavVideoSource("input.mkv").ConvertToYV12()
sd  = src.SCDetectEOD(debug=true)

ScriptClip(sd, """
  b  = propGetInt("_scd_boundary")
  hy = propGetInt("_scd_dHistY")
  hu = propGetInt("_scd_dHistUV")
  iy = propGetInt("_scd_intY")
  iu = propGetInt("_scd_intUV")
  t8  = propGetInt("_scd_jTop8")
  t16 = propGetInt("_scd_jTop16")
  dm = propGetInt("_scd_dMeanY")
  fl = propGetInt("_scd_isFlicker")
  my = propGetInt("_scd_meanY")

  marker = (b == 1) ? "  *** CUT ***" : ""
  flag   = (fl == 1) ? "  [FLICKER]"  : ""

  Subtitle("f" + String(current_frame) + "  meanY=" + String(my) + marker + flag, y=0,   size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("dHistY="  + String(hy),  y=26,  size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("dHistUV=" + String(hu),  y=50,  size=20, text_color=$ffff00, halo_color=$000000)
  Subtitle("intY="    + String(iy),  y=74,  size=20, text_color=$00ff00, halo_color=$000000)
  Subtitle("intUV="   + String(iu),  y=98,  size=20, text_color=$00ff00, halo_color=$000000)
  Subtitle("jTop8="   + String(t8),  y=122, size=20, text_color=$ff88ff, halo_color=$000000)
  Subtitle("jTop16="  + String(t16), y=146, size=20, text_color=$ff88ff, halo_color=$000000)
  Subtitle("dMeanY="  + String(dm),  y=170, size=20, text_color=$ffff00, halo_color=$000000)
""")
