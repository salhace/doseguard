// DoseGuard cap enclosure — 3D-printable (PETG recommended)
// Fits a standard disposable insulin pen (~16.5–17.5 mm body). Adjust pen_d to your pen.
// Author: Salha Alwahbi · MIT

$fn = 96;

pen_d      = 17.6;   // pen body diameter + clearance
pen_depth  = 52;     // how far the pen slides into the cap
wall       = 2.0;
len        = 70;     // overall cap length
bay_w      = 30;     // electronics bay (ESP32-C3 mini / OLED module)
bay_h      = 14;
oled_win   = [24, 12.5];   // 0.96" OLED visible area
led_d      = 5.2;
btn_d      = 6.6;

module rounded_box(size, r) {
  hull() for (x = [r, size[0]-r], y = [r, size[1]-r]) translate([x, y, 0]) cylinder(r = r, h = size[2]);
}

module shell() {
  // pen sleeve
  rotate([0, 90, 0]) cylinder(d = pen_d + 2*wall, h = len);
  // electronics bay on top of the sleeve
  translate([0, -bay_w/2, pen_d/2 - 3]) rotate([0, 0, 0])
    rounded_box([len, bay_w, bay_h], 5);
}

module cuts() {
  // pen bore (open at x = len)
  translate([len - pen_depth, 0, 0]) rotate([0, 90, 0]) cylinder(d = pen_d, h = pen_depth + 1);
  // needle pocket
  rotate([0, 90, 0]) translate([0, 0, wall]) cylinder(d = 9, h = len - pen_depth);
  // electronics cavity
  translate([wall, -bay_w/2 + wall, pen_d/2 - 3 + wall]) cube([len - 2*wall, bay_w - 2*wall, bay_h - 2*wall]);
  // OLED window
  translate([10, -oled_win[1]/2, pen_d/2 - 3 + bay_h - wall - 1]) cube([oled_win[0], oled_win[1], wall + 2]);
  // status LED
  translate([44, 0, pen_d/2 - 3 + bay_h - wall - 1]) cylinder(d = led_d, h = wall + 2);
  // button
  translate([56, 0, pen_d/2 - 3 + bay_h - wall - 1]) cylinder(d = btn_d, h = wall + 2);
  // cap-detect: slot for a reed switch; small magnet goes on the pen body
  translate([len - 10, -2, -pen_d/2 - wall - 1]) cube([7, 4, wall]);
  // thermistor port close to the insulin cartridge
  translate([len - pen_depth + 6, 0, -pen_d/2 - wall - 1]) cylinder(d = 3.2, h = wall + 2);
  // USB-C charge port at the closed end
  translate([-1, -4.5, pen_d/2 + 1]) cube([wall + 2, 9, 3.5]);
}

color("#e8ecf2") difference() { shell(); cuts(); }
