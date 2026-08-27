#!/usr/bin/env python3
"""
ecofloat_mass_budget.py
=======================

Mass budget and flotation solver for EcoFloat V3.

Two jobs:
  1. Build an all-up mass estimate from the BOM, tracking which numbers are
     measured, which are manufacturer-published, and which are guesses.
  2. Solve the flotation problem -- given that mass, how deep does the boat
     sit and how much freeboard is left?

The point of separating (1) and (2) is that the flotation answer is only as
good as the mass estimate, and the mass estimate is mostly guesses until you
put the parts on a scale. The SOURCE column is there so you can see exactly
how much of your answer rests on made-up numbers.

Run:  python3 ecofloat_mass_budget.py

Author: Ahaan Anand
"""

from dataclasses import dataclass, field
from typing import Optional

# ---------------------------------------------------------------------------
# CONSTANTS
# ---------------------------------------------------------------------------

G = 9.80665                # m/s^2
RHO_FRESHWATER_20C = 998.2 # kg/m^3, freshwater at 20 C
LB_PER_KG = 2.20462

# Source tags -- how much do we trust each number?
MEASURED  = "MEASURED"   # you put it on a scale
PUBLISHED = "PUBLISHED"  # manufacturer spec sheet
ESTIMATED = "ESTIMATED"  # educated guess, REPLACE ME


# ---------------------------------------------------------------------------
# COMPONENT MODEL
# ---------------------------------------------------------------------------

@dataclass
class Component:
    """A discrete item with a mass. qty is how many are ON THE BOAT."""
    name: str
    qty: float
    unit_mass_g: float
    source: str
    group: str
    note: str = ""

    @property
    def total_g(self) -> float:
        return self.qty * self.unit_mass_g


@dataclass
class AreaMaterial:
    """
    A material applied over an area -- plywood, glass cloth, epoxy, paint.

    These are NOT costed the way the BOM costs them. The BOM says "1x plywood,
    $20" because that's one sheet from the hardware store. But you don't
    install a whole sheet, you install however many square metres of panel the
    hull actually needs. Purchased quantity and installed mass are different
    numbers, and conflating them is the most common way a mass budget goes
    wrong.
    """
    name: str
    area_m2: float
    areal_density_kg_m2: float
    source: str
    group: str
    note: str = ""

    @property
    def total_g(self) -> float:
        return self.area_m2 * self.areal_density_kg_m2 * 1000.0


# ---------------------------------------------------------------------------
# HULL GEOMETRY  --  EDIT THESE FROM YOUR FUSION MODEL
# ---------------------------------------------------------------------------

@dataclass
class HullGeometry:
    """
    Per-hull geometry for one hull of the catamaran.

    Pull these straight out of Fusion. The waterplane beam is the hull width
    at the waterline, which is NOT the same as max beam on a flared hull --
    measure it at the height you expect the boat to float at.
    """
    n_hulls: int = 2
    length_waterline_m: float = 1.20   # LWL per hull   <-- FROM FUSION
    beam_waterline_m: float = 0.16     # BWL per hull   <-- FROM FUSION
    hull_depth_m: float = 0.28         # keel to deck   <-- FROM FUSION
    block_coefficient: float = 0.65    # see note below

    # Block coefficient (Cb) corrects the rectangular-prism approximation for
    # the fact that a real hull tapers at bow and stern and is rounded at the
    # bilge. A slender displacement hull runs roughly 0.5-0.65; a barge is
    # near 1.0. Your V3 is deliberately slender, so the low end applies.
    # If you want the exact number, Fusion will give you the submerged volume
    # directly via a section analysis -- use that instead of this estimate.

    @property
    def waterplane_area_m2(self) -> float:
        """Total waterplane area across all hulls."""
        return self.n_hulls * self.length_waterline_m * self.beam_waterline_m

    def submerged_volume_at_draft(self, draft_m: float) -> float:
        """Displaced volume when the hull sits draft_m below the surface."""
        draft_m = max(0.0, min(draft_m, self.hull_depth_m))
        return (self.n_hulls
                * self.length_waterline_m
                * self.beam_waterline_m
                * draft_m
                * self.block_coefficient)

    @property
    def total_hull_volume_m3(self) -> float:
        """Full hull volume if submerged to the deck -- your reserve buoyancy ceiling."""
        return self.submerged_volume_at_draft(self.hull_depth_m)


# ---------------------------------------------------------------------------
# THE BOM
# ---------------------------------------------------------------------------
#
# qty here = QUANTITY ON THE BOAT, which is not always the BOM quantity.
# The BOM buys a 2-pack of Heltec boards; only one of them goes to sea. The
# transmitter never leaves your hands. Shore-side items are listed at qty=0
# so they stay visible in the table without polluting the total.

COMPONENTS = [
    # --- Science payload ---------------------------------------------------
    Component("DFRobot SEN0680 DO probe (body)", 1, 400.0, ESTIMATED, "Payload",
              "316SS + plastic body. No mass on DFRobot spec sheet -- WEIGH THIS."),
    Component("SEN0680 cable (5 m attached)", 1, 250.0, ESTIMATED, "Payload",
              "5 m of 4-conductor at ~50 g/m. Easy to forget; it's 1/4 lb of wire."),
    Component("DFR0845 RS485-to-UART adapter", 1, 15.0, ESTIMATED, "Payload"),
    Component("Heltec WiFi LoRa 32 V3 (boat node)", 1, 15.0, ESTIMATED, "Payload",
              "Vendor listings say 35 g -- that's the shipping box. Bare board ~15 g."),
    Component("915 MHz LoRa antenna", 1, 15.0, ESTIMATED, "Payload"),
    Component("Betian BK-250P GPS/compass", 1, 30.0, ESTIMATED, "Payload",
              "Includes mast mount hardware."),
    Component("Sensor-pod LiPo", 1, 50.0, ESTIMATED, "Payload"),

    # --- Propulsion --------------------------------------------------------
    Component("ApisQueen U2 thruster", 2, 210.0, PUBLISHED, "Propulsion",
              "Vendor spec: 210 g. Check whether that includes the >900 mm lead."),
    Component("DH 40A bidirectional ESC", 2, 40.0, ESTIMATED, "Propulsion"),
    Component("3S LiPo (propulsion)", 2, 400.0, ESTIMATED, "Propulsion",
              "Assumes ~5000 mAh 3S. Scales ~linearly with capacity -- "
              "biggest single lever in the whole budget."),

    # --- Avionics ----------------------------------------------------------
    Component("CoreWing F405 Wing V2 stack", 1, 50.0, ESTIMATED, "Avionics",
              "3-board stack: FC + PDB + wireless extender. "
              "The 180 g figure on AliExpress is packaged shipping weight."),
    Component("ELRS receiver", 1, 5.0, ESTIMATED, "Avionics"),

    # --- Structure and rigging ---------------------------------------------
    Component("Wiring harness (total installed)", 1, 250.0, ESTIMATED, "Structure",
              "Power runs dominate; signal wire is negligible by comparison."),
    Component("Screws and fasteners (installed)", 1, 150.0, ESTIMATED, "Structure",
              "You bought $29 worth; only the installed subset counts."),
    Component("Metal rods / structural spars", 1, 200.0, ESTIMATED, "Structure"),
    Component("Recovery rope (carried aboard)", 1, 100.0, ESTIMATED, "Structure",
              "Set qty=0 if the tether stays on shore during autonomous runs."),
    Component("3D printed parts (bilge, trays, mounts)", 1, 500.0, ESTIMATED, "Structure",
              "Resin + FDM combined. Weigh the printed parts -- you have them "
              "in hand, so this guess is the easiest one to eliminate."),

    # --- Shore side -- carried at qty 0 so they don't sneak into the total --
    Component("Heltec V3 (shore base station)", 0, 15.0, ESTIMATED, "Shore",
              "Second board from the 2-pack. Stays on the bank."),
    Component("RadioMaster T8L transmitter", 0, 300.0, ESTIMATED, "Shore",
              "In your hands, not on the boat."),
]


# --- Hull composite: computed from area, not from purchase quantity --------
#
# Areal densities:
#   Marine plywood ~600 kg/m^3. At 2.7 mm that's 1.62 kg/m^2; at 1/4"
#   (6.35 mm) it's 3.81 kg/m^2. NOTE: your BOM says 1/4 inch but the CAD
#   devlog says 2.7 mm. Those differ by 2.35x in mass. Resolve this before
#   trusting any number below.
#
#   Fiberglass layup: 6 oz cloth is ~200 g/m^2 dry. Hand layup absorbs roughly
#   its own weight in epoxy, so ~400-450 g/m^2 per layer wet-out. Two layers
#   over the whole exterior is a reasonable default.

PLYWOOD_THICKNESS_M = 0.0027     # <-- 0.0027 for 2.7 mm, 0.00635 for 1/4"
PLYWOOD_DENSITY_KG_M3 = 600.0

HULL_WETTED_AREA_M2 = 1.80       # <-- FROM FUSION: total exterior area, both hulls
DECK_AREA_M2 = 0.70              # <-- FROM FUSION

AREA_MATERIALS = [
    AreaMaterial("Plywood hull panels",
                 HULL_WETTED_AREA_M2 + DECK_AREA_M2,
                 PLYWOOD_THICKNESS_M * PLYWOOD_DENSITY_KG_M3,
                 ESTIMATED, "Hull",
                 "Areal density = thickness x density. Check the 2.7 mm vs 1/4 in conflict."),
    AreaMaterial("Fiberglass + epoxy, 2 layers exterior",
                 HULL_WETTED_AREA_M2,
                 2 * 0.425,
                 ESTIMATED, "Hull",
                 "6 oz cloth at ~200 g/m^2 dry, ~1:1 epoxy pickup in hand layup."),
    AreaMaterial("Fairing compound",
                 HULL_WETTED_AREA_M2,
                 0.30,
                 ESTIMATED, "Hull",
                 "Fills the plywood-to-printed-bilge transition. Sanding removes "
                 "much of what you apply, so this is applied-minus-sanded."),
    AreaMaterial("Marine paint (2 coats)",
                 HULL_WETTED_AREA_M2 + DECK_AREA_M2,
                 0.25,
                 ESTIMATED, "Hull"),
]


# ---------------------------------------------------------------------------
# MASS ROLL-UP
# ---------------------------------------------------------------------------

def total_mass_kg(components, area_materials) -> float:
    g = sum(c.total_g for c in components)
    g += sum(m.total_g for m in area_materials)
    return g / 1000.0


def mass_by_group(components, area_materials) -> dict:
    groups = {}
    for c in components:
        if c.qty > 0:
            groups[c.group] = groups.get(c.group, 0.0) + c.total_g
    for m in area_materials:
        groups[m.group] = groups.get(m.group, 0.0) + m.total_g
    return groups


def mass_by_source(components, area_materials) -> dict:
    """How much of the total rests on guesses? This is the honesty check."""
    sources = {}
    for c in components:
        if c.qty > 0:
            sources[c.source] = sources.get(c.source, 0.0) + c.total_g
    for m in area_materials:
        sources[m.source] = sources.get(m.source, 0.0) + m.total_g
    return sources


# ---------------------------------------------------------------------------
# FLOTATION
# ---------------------------------------------------------------------------

def displaced_volume_m3(mass_kg: float, rho: float = RHO_FRESHWATER_20C) -> float:
    """
    Archimedes. At equilibrium, buoyant force equals weight:

        rho * g * V = m * g   ->   V = m / rho

    g cancels, so displaced volume depends only on mass and water density.
    """
    return mass_kg / rho


def solve_draft(mass_kg: float, hull: HullGeometry,
                rho: float = RHO_FRESHWATER_20C) -> float:
    """
    Find the draft at which displaced volume equals the required volume.

    With a prismatic hull this is closed-form, but bisection is used here so
    the function still works if you swap in a non-linear volume curve from
    Fusion later.
    """
    v_required = displaced_volume_m3(mass_kg, rho)

    if v_required > hull.total_hull_volume_m3:
        return float("nan")   # sinks: no draft satisfies the equation

    lo, hi = 0.0, hull.hull_depth_m
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if hull.submerged_volume_at_draft(mid) < v_required:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


def freeboard_m(mass_kg: float, hull: HullGeometry,
                rho: float = RHO_FRESHWATER_20C) -> float:
    """Air draft: hull height above the waterline."""
    return hull.hull_depth_m - solve_draft(mass_kg, hull, rho)


def reserve_buoyancy_ratio(mass_kg: float, hull: HullGeometry,
                           rho: float = RHO_FRESHWATER_20C) -> float:
    """
    Total hull volume divided by displaced volume.

    How many times its own displacement the boat could take on before the
    deck goes under. Higher is safer.
    """
    return hull.total_hull_volume_m3 / displaced_volume_m3(mass_kg, rho)


def swamping_margin_kg(mass_kg: float, hull: HullGeometry,
                       rho: float = RHO_FRESHWATER_20C) -> float:
    """Extra mass the boat could take on before the deck reaches the waterline."""
    return hull.total_hull_volume_m3 * rho - mass_kg


def mass_for_target_freeboard(target_freeboard_m: float, hull: HullGeometry,
                              rho: float = RHO_FRESHWATER_20C) -> float:
    """
    Inverse problem: what all-up mass hits a given air draft?

    This is the number that should have driven the design. Run it for 0.2 m
    and compare against what the boat actually weighs.
    """
    draft = hull.hull_depth_m - target_freeboard_m
    if draft <= 0:
        return 0.0
    return hull.submerged_volume_at_draft(draft) * rho


# ---------------------------------------------------------------------------
# REPORTING
# ---------------------------------------------------------------------------

def print_bom_table(components, area_materials):
    print("=" * 78)
    print("MASS BUDGET")
    print("=" * 78)
    print(f"{'Item':<42}{'Qty':>5}{'Unit g':>9}{'Total g':>10}{'Src':>12}")
    print("-" * 78)

    current = None
    for c in sorted(components, key=lambda x: (x.group, -x.total_g)):
        if c.group != current:
            current = c.group
            print(f"\n[{current}]")
        flag = "" if c.qty > 0 else "  (not aboard)"
        print(f"{c.name[:42]:<42}{c.qty:>5.0f}{c.unit_mass_g:>9.1f}"
              f"{c.total_g:>10.1f}{c.source:>12}{flag}")

    print(f"\n[Hull composite -- computed from area]")
    for m in area_materials:
        print(f"{m.name[:42]:<42}{m.area_m2:>5.2f}{m.areal_density_kg_m2*1000:>9.0f}"
              f"{m.total_g:>10.1f}{m.source:>12}")
    print(f"{'  (Qty column = area in m^2, Unit = g/m^2)':<42}")


def print_summary(components, area_materials, hull, rho=RHO_FRESHWATER_20C):
    total_kg = total_mass_kg(components, area_materials)
    total_lb = total_kg * LB_PER_KG

    print("\n" + "=" * 78)
    print("BY SUBSYSTEM")
    print("=" * 78)
    groups = mass_by_group(components, area_materials)
    for name, grams in sorted(groups.items(), key=lambda kv: -kv[1]):
        pct = 100.0 * grams / (total_kg * 1000.0)
        bar = "#" * int(pct / 2)
        print(f"{name:<16}{grams:>9.0f} g{pct:>7.1f}%  {bar}")

    print("\n" + "=" * 78)
    print("CONFIDENCE")
    print("=" * 78)
    sources = mass_by_source(components, area_materials)
    for name, grams in sorted(sources.items(), key=lambda kv: -kv[1]):
        pct = 100.0 * grams / (total_kg * 1000.0)
        print(f"{name:<16}{grams:>9.0f} g{pct:>7.1f}%")
    est_pct = 100.0 * sources.get(ESTIMATED, 0) / (total_kg * 1000.0)
    print(f"\n{est_pct:.0f}% of this budget is guesswork. Every ESTIMATED row is")
    print("a number you could replace with a kitchen scale in about an hour.")

    print("\n" + "=" * 78)
    print("ALL-UP MASS")
    print("=" * 78)
    print(f"  Computed:            {total_kg:>7.2f} kg  ({total_lb:.1f} lb)")
    print(f"  Design assumption:   {18.14:>7.2f} kg  (40.0 lb)")
    delta = total_lb - 40.0
    print(f"  Delta:               {delta:>+7.1f} lb  ({100*delta/40.0:+.0f}%)")

    print("\n" + "=" * 78)
    print("FLOTATION")
    print("=" * 78)
    v_disp = displaced_volume_m3(total_kg, rho)
    draft = solve_draft(total_kg, hull, rho)
    fb = freeboard_m(total_kg, hull, rho)

    print(f"  Water density:        {rho:>7.1f} kg/m^3")
    print(f"  Displaced volume:     {v_disp:>7.4f} m^3   ({v_disp*1000:.1f} L)")
    print(f"    per hull:           {v_disp/hull.n_hulls:>7.4f} m^3   "
          f"({v_disp*1000/hull.n_hulls:.1f} L)")
    print(f"  Total hull volume:    {hull.total_hull_volume_m3:>7.4f} m^3")

    # Cross-check against the volume target quoted in the CAD devlog.
    v_target = 0.150
    v_computed = hull.total_hull_volume_m3
    ratio = v_computed / v_target
    if not (0.85 < ratio < 1.15):
        print()
        print(f"  !! GEOMETRY MISMATCH")
        print(f"     Devlog states a 0.150 m^3 hull volume target.")
        print(f"     This geometry gives {v_computed:.4f} m^3 ({ratio:.2f}x the target).")
        print(f"     Either the HullGeometry placeholders are wrong, or the 0.150 m^3")
        print(f"     figure includes something this prismatic model doesn't. Resolve")
        print(f"     it before quoting either number in a paper.")
    print()

    if draft != draft:  # NaN
        print("  *** SINKS *** Required displacement exceeds hull volume.")
        return

    print(f"  Draft:                {draft:>7.3f} m   ({draft*39.37:.1f} in)")
    print(f"  Freeboard:            {fb:>7.3f} m   ({fb*39.37:.1f} in)")
    print(f"  Target freeboard:     {0.200:>7.3f} m   (V1/V2 were 0.040 m)")
    print(f"  Reserve buoyancy:     {reserve_buoyancy_ratio(total_kg, hull, rho):>7.2f}x")
    print(f"  Swamping margin:      {swamping_margin_kg(total_kg, hull, rho):>7.2f} kg "
          f"of water before the deck goes under")
    print()

    if fb >= 0.20:
        print(f"  PASS -- {fb:.3f} m clears the 0.2 m target.")
    elif fb >= 0.10:
        print(f"  MARGINAL -- {fb:.3f} m is well above V2's 0.04 m but short of target.")
    else:
        print(f"  FAIL -- {fb:.3f} m is V2 territory. This is the geometry that swamped.")

    budget = mass_for_target_freeboard(0.20, hull, rho)
    print(f"\n  Mass budget for 0.2 m freeboard: {budget:.2f} kg ({budget*LB_PER_KG:.1f} lb)")
    print(f"  Currently over/under by:         {total_kg - budget:+.2f} kg "
          f"({(total_kg-budget)*LB_PER_KG:+.1f} lb)")


def print_sensitivity(hull, rho=RHO_FRESHWATER_20C):
    """
    How much does freeboard move as mass moves?

    Worth running because it tells you whether the estimate needs to be
    accurate to 100 g or 2 kg. On a wide, shallow hull a kilo barely
    registers; on a slender one it's centimetres.
    """
    print("\n" + "=" * 78)
    print("SENSITIVITY -- freeboard vs all-up mass")
    print("=" * 78)
    print(f"{'Mass (kg)':>11}{'Mass (lb)':>11}{'Draft (m)':>12}{'Freeboard (m)':>16}")
    print("-" * 78)
    for mass in [8, 10, 12, 14, 16, 18, 20, 22, 25, 30]:
        d = solve_draft(mass, hull, rho)
        f = freeboard_m(mass, hull, rho)
        if d != d:
            print(f"{mass:>11.1f}{mass*LB_PER_KG:>11.1f}{'--':>12}{'SINKS':>16}")
        else:
            mark = "  <-- target" if abs(f - 0.20) < 0.005 else ""
            print(f"{mass:>11.1f}{mass*LB_PER_KG:>11.1f}{d:>12.3f}{f:>16.3f}{mark}")

    wp = hull.waterplane_area_m2
    sink_rate = 1.0 / (wp * rho) * 1000  # mm of draft per kg
    print(f"\n  Waterplane area: {wp:.3f} m^2")
    print(f"  Sinkage rate:    {sink_rate:.1f} mm of draft per kg added")
    print(f"  So a 1 kg estimate error moves the waterline {sink_rate:.0f} mm.")


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
    hull = HullGeometry()

    print_bom_table(COMPONENTS, AREA_MATERIALS)
    print_summary(COMPONENTS, AREA_MATERIALS, hull)
    print_sensitivity(hull)

    print("\n" + "=" * 78)
    print("WHAT TO FIX FIRST")
    print("=" * 78)
    print("""
  1. Resolve the plywood thickness conflict. The BOM says 1/4 inch, the CAD
     devlog says 2.7 mm. That is a 2.35x difference on the single largest
     line in the budget.

  2. Put the printed parts on a scale. You have them. The 500 g guess is
     the biggest unmeasured item and it takes two minutes to eliminate.

  3. Get the real LiPo mass. Two packs at an assumed 400 g each is 800 g of
     pure assumption, and it scales directly with the capacity you chose.

  4. Replace HULL_WETTED_AREA_M2 and DECK_AREA_M2 with Fusion's surface-area
     measurement rather than the placeholders here.

  5. Then float the boat, mark the waterline, and compare. That measured
     number is what makes this a validated model instead of a spreadsheet.
""")


if __name__ == "__main__":
    main()
