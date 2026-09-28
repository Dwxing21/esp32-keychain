// ============================================================================
// Keychain case for the Waveshare ESP32-S3-LCD-1.28 (round GC9A01, no touch)
// ============================================================================
//
// IMPORTANT -- read before printing:
// The display opening (32.4mm) is a solid number: a "1.28 inch" round panel
// is 32.51mm across by definition, so that part is trustworthy.
// The PCB outer diameter, and WHERE the USB-C port / BOOT button / battery
// connector sit, are NOT things I have a verified drawing for -- Waveshare
// only publishes those as a PDF image, not machine-readable numbers. Also
// note: the BOOT button is mounted on the board's BACK face (confirmed), not
// its edge, so its access hole goes through the case's back floor, not the
// side wall like the USB-C port does. Before printing:
//   1. Measure your actual board's diameter with calipers and update
//      board_diameter below.
//   2. With the board back-side up and USB-C pointing toward you (6 o'clock),
//      measure the BOOT button's position from board center and update
//      boot_x/boot_y. Note the USB-C port's edge position too and update
//      usb_c_angle if needed.
//   3. Print the BODY alone first in draft quality (thick layers, low infill)
//      as a fit test before committing filament to a nicer finish.
//
// Prints as two parts -- set part below, or "both" just to look at them
// together (don't print "both" as one plate).

part = "body";  // "body" | "lid" | "both"

// ---- Board dimensions (VERIFY board_diameter against your real board) ----
board_diameter   = 37.6;  // outer edge of the round PCB, mm -- ESTIMATE, VERIFY
display_active_d = 32.4;  // "1.28 inch" round glass = 32.51mm -- this one's solid
board_thickness  = 1.6;   // standard PCB thickness
component_clearance = 5;  // depth below the board for USB-C jack / battery connector / any header pins

// ---- USB-C cutout on the case wall (VERIFY POSITION) ----
usb_c_width  = 10;
usb_c_height = 4;
usb_c_angle  = 270;   // degrees around the case where the port sits (270 = "6 o'clock")

// ---- BOOT button access hole (VERIFY POSITION) ----
// This board's BOOT button is mounted on the board's BACK face, not its edge --
// so this drills a vertical hole through the case's back FLOOR, not the side
// wall. To find boot_x/boot_y: look at the back of your board with the USB-C
// port pointing toward you (6 o'clock), then measure the button's center from
// the board's middle, left/right and up/down, in mm. The values below are a
// placeholder guess, not a measurement.
boot_hole_d = 3.2;
boot_x = 0;   // mm from board center, left(-) / right(+) -- ESTIMATE, VERIFY
boot_y = 8;   // mm from board center, away-from-USB-C(+) / toward-it(-) -- ESTIMATE, VERIFY

// ---- Battery connector wire slot, from the pocket up to the board ----
batt_conn_angle = 180; // 180 = "9 o'clock" -- opposite the USB-C port by default

// ---- Battery compartment (behind the board) ----
// Sized for the Grobotronics 3.7V 620mAh Molex-1.25mm cell (40x20x8.5mm),
// plus a little clearance for an easy fit and wire routing.
batt_length = 42;
batt_width  = 22;
batt_thickness = 9.5;

// ---- Case shell ----
wall = 2.0;
lid_lip = 1.2;         // how far the lid's inner lip plugs into the body
fit_clearance = 0.4;   // extra radius so the board isn't press-fit
keychain_loop_d = 5.5; // inner hole diameter for a keyring
keychain_loop_wall = 2.5;

$fn = 96;

// This battery's footprint (40x20mm) is close to the board's own diameter, so a
// case sized only to the board can't also enclose it -- the case grows to fit
// whichever is larger. With the 620mAh cell above, the battery is what drives
// the size: expect a ~52mm case, not a ~42mm one.
board_based_diameter = board_diameter + 2*fit_clearance + 2*wall;
battery_based_diameter = sqrt(pow(batt_length, 2) + pow(batt_width, 2)) + 2*wall + 2*fit_clearance;
case_diameter = max(board_based_diameter, battery_based_diameter);
body_depth = component_clearance + batt_thickness + wall;
lid_depth = board_thickness + 2.5;

// ---------------------------------------------------------------------------

module keychain_loop(h) {
    r_outer = keychain_loop_d/2 + keychain_loop_wall;
    translate([0, case_diameter/2 + r_outer - 1.5, h/2])
        difference() {
            cylinder(h = h, r = r_outer, center = true);
            cylinder(h = h + 2, r = keychain_loop_d/2, center = true);
        }
}

module body() {
    difference() {
        union() {
            cylinder(h = body_depth, d = case_diameter);
            keychain_loop(body_depth);
        }
        // hollow interior -- the battery just sits loose in here (secure it with a
        // strip of foam or double-sided tape), no separate pocket cut needed now
        // that it's roughly the same footprint as the board itself
        translate([0, 0, wall])
            cylinder(h = body_depth, d = case_diameter - 2*wall);

        // USB-C cutout through the wall
        rotate([0, 0, usb_c_angle])
            translate([0, case_diameter/2 - wall/2, body_depth - component_clearance/2])
                cube([usb_c_width, wall*3, usb_c_height], center = true);

        // BOOT button access hole through the BACK FLOOR (vertical) -- this
        // board's button is back-mounted, not edge-mounted, unlike the USB-C
        // port above which genuinely is on the edge
        translate([boot_x, boot_y, -1])
            cylinder(h = wall + 2, d = boot_hole_d);

        // slot for the battery connector wire to reach the board
        // (this one's still assumed edge-mounted -- worth double-checking
        // the same way you caught the button, since I haven't verified it either)
        rotate([0, 0, batt_conn_angle])
            translate([0, case_diameter/2 - wall/2, wall + batt_thickness/2])
                cube([6, wall*3, batt_thickness], center = true);
    }
}

module lid() {
    difference() {
        union() {
            cylinder(h = lid_depth, d = case_diameter);
            // inner lip that plugs into the body for a friction-fit close
            translate([0, 0, -lid_lip])
                cylinder(h = lid_lip, d = case_diameter - 2*wall - 0.4);
        }
        // display window
        translate([0, 0, -1])
            cylinder(h = lid_depth + 2, d = display_active_d);
        // thin the lip so it isn't one solid slab on top of the display
        translate([0, 0, 1.5])
            cylinder(h = lid_depth, d = case_diameter - 2*wall - 2);
    }
}

if (part == "body") body();
if (part == "lid") lid();
if (part == "both") {
    body();
    translate([case_diameter + 12, 0, 0]) lid();
}
