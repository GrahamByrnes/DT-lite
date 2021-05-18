typedef struct
{
  unsigned long int wavelength;
  cmsCIEXYZ xyz;
} _cie_colorimetric_observers;

/*
 * CIE 1931 2° standard colorimetric observer
 * https://en.wikipedia.org/wiki/CIE_1931_color_space#CIE_standard_observer
 *
 * Origin:
 * https://en.wikipedia.org/wiki/Illuminant_D65#External_links
 * -> http://www.cie.co.at/publ/abst/datatables15_2004/CIE_sel_colorimetric_tables.xls "1931 col observer"
 */
static const _cie_colorimetric_observers cie_1931_std_colorimetric_observer[] = { //
  { 380, { 0.001368, 0.000039, 0.006450 } },
  { 385, { 0.002236, 0.000064, 0.010550 } },
  { 390, { 0.004243, 0.000120, 0.020050 } },
  { 395, { 0.007650, 0.000217, 0.036210 } },
  { 400, { 0.014310, 0.000396, 0.067850 } },
  { 405, { 0.023190, 0.000640, 0.110200 } },
  { 410, { 0.043510, 0.001210, 0.207400 } },
  { 415, { 0.077630, 0.002180, 0.371300 } },
  { 420, { 0.134380, 0.004000, 0.645600 } },
  { 425, { 0.214770, 0.007300, 1.039050 } },
  { 430, { 0.283900, 0.011600, 1.385600 } },
  { 435, { 0.328500, 0.016840, 1.622960 } },
  { 440, { 0.348280, 0.023000, 1.747060 } },
  { 445, { 0.348060, 0.029800, 1.782600 } },
  { 450, { 0.336200, 0.038000, 1.772110 } },
  { 455, { 0.318700, 0.048000, 1.744100 } },
  { 460, { 0.290800, 0.060000, 1.669200 } },
  { 465, { 0.251100, 0.073900, 1.528100 } },
  { 470, { 0.195360, 0.090980, 1.287640 } },
  { 475, { 0.142100, 0.112600, 1.041900 } },
  { 480, { 0.095640, 0.139020, 0.812950 } },
  { 485, { 0.057950, 0.169300, 0.616200 } },
  { 490, { 0.032010, 0.208020, 0.465180 } },
  { 495, { 0.014700, 0.258600, 0.353300 } },
  { 500, { 0.004900, 0.323000, 0.272000 } },
  { 505, { 0.002400, 0.407300, 0.212300 } },
  { 510, { 0.009300, 0.503000, 0.158200 } },
  { 515, { 0.029100, 0.608200, 0.111700 } },
  { 520, { 0.063270, 0.710000, 0.078250 } },
  { 525, { 0.109600, 0.793200, 0.057250 } },
  { 530, { 0.165500, 0.862000, 0.042160 } },
  { 535, { 0.225750, 0.914850, 0.029840 } },
  { 540, { 0.290400, 0.954000, 0.020300 } },
  { 545, { 0.359700, 0.980300, 0.013400 } },
  { 550, { 0.433450, 0.994950, 0.008750 } },
  { 555, { 0.512050, 1.000000, 0.005750 } },
  { 560, { 0.594500, 0.995000, 0.003900 } },
  { 565, { 0.678400, 0.978600, 0.002750 } },
  { 570, { 0.762100, 0.952000, 0.002100 } },
  { 575, { 0.842500, 0.915400, 0.001800 } },
  { 580, { 0.916300, 0.870000, 0.001650 } },
  { 585, { 0.978600, 0.816300, 0.001400 } },
  { 590, { 1.026300, 0.757000, 0.001100 } },
  { 595, { 1.056700, 0.694900, 0.001000 } },
  { 600, { 1.062200, 0.631000, 0.000800 } },
  { 605, { 1.045600, 0.566800, 0.000600 } },
  { 610, { 1.002600, 0.503000, 0.000340 } },
  { 615, { 0.938400, 0.441200, 0.000240 } },
  { 620, { 0.854450, 0.381000, 0.000190 } },
  { 625, { 0.751400, 0.321000, 0.000100 } },
  { 630, { 0.642400, 0.265000, 0.000050 } },
  { 635, { 0.541900, 0.217000, 0.000030 } },
  { 640, { 0.447900, 0.175000, 0.000020 } },
  { 645, { 0.360800, 0.138200, 0.000010 } },
  { 650, { 0.283500, 0.107000, 0.000000 } },
  { 655, { 0.218700, 0.081600, 0.000000 } },
  { 660, { 0.164900, 0.061000, 0.000000 } },
  { 665, { 0.121200, 0.044580, 0.000000 } },
  { 670, { 0.087400, 0.032000, 0.000000 } },
  { 675, { 0.063600, 0.023200, 0.000000 } },
  { 680, { 0.046770, 0.017000, 0.000000 } },
  { 685, { 0.032900, 0.011920, 0.000000 } },
  { 690, { 0.022700, 0.008210, 0.000000 } },
  { 695, { 0.015840, 0.005723, 0.000000 } },
  { 700, { 0.011359, 0.004102, 0.000000 } },
  { 705, { 0.008111, 0.002929, 0.000000 } },
  { 710, { 0.005790, 0.002091, 0.000000 } },
  { 715, { 0.004109, 0.001484, 0.000000 } },
  { 720, { 0.002899, 0.001047, 0.000000 } },
  { 725, { 0.002049, 0.000740, 0.000000 } },
  { 730, { 0.001440, 0.000520, 0.000000 } },
  { 735, { 0.001000, 0.000361, 0.000000 } },
  { 740, { 0.000690, 0.000249, 0.000000 } },
  { 745, { 0.000476, 0.000172, 0.000000 } },
  { 750, { 0.000332, 0.000120, 0.000000 } },
  { 755, { 0.000235, 0.000085, 0.000000 } },
  { 760, { 0.000166, 0.000060, 0.000000 } },
  { 765, { 0.000117, 0.000042, 0.000000 } },
  { 770, { 0.000083, 0.000030, 0.000000 } },
  { 775, { 0.000059, 0.000021, 0.000000 } },
  { 780, { 0.000042, 0.000015, 0.000000 } }
};

static const size_t cie_1931_std_colorimetric_observer_count = sizeof(cie_1931_std_colorimetric_observer) / sizeof(_cie_colorimetric_observers);

typedef struct
{
  unsigned long int wavelength;
  double S[3];
} _cie_std_daylight_component;

/*
 * Daylight components
 *
 * Origin:
 * https://en.wikipedia.org/wiki/Illuminant_D65#External_links
 * -> http://www.cie.co.at/publ/abst/datatables15_2004/CIE_sel_colorimetric_tables.xls "Daylight comp"
 * Or from http://www.brucelindbloom.com/Eqn_DIlluminant.html
 * -> http://www.brucelindbloom.com/downloads/DIlluminants.xls.zip
 */
static const _cie_std_daylight_component cie_daylight_components[] = { //
  { 300, { 0.04, 0.02, 0.00 } },
  { 305, { 3.02, 2.26, 1.00 } },
  { 310, { 6.00, 4.50, 2.00 } },
  { 315, { 17.80, 13.45, 3.00 } },
  { 320, { 29.60, 22.40, 4.00 } },
  { 325, { 42.45, 32.20, 6.25 } },
  { 330, { 55.30, 42.00, 8.50 } },
  { 335, { 56.30, 41.30, 8.15 } },
  { 340, { 57.30, 40.60, 7.80 } },
  { 345, { 59.55, 41.10, 7.25 } },
  { 350, { 61.80, 41.60, 6.70 } },
  { 355, { 61.65, 39.80, 6.00 } },
  { 360, { 61.50, 38.00, 5.30 } },
  { 365, { 65.15, 40.20, 5.70 } },
  { 370, { 68.80, 42.40, 6.10 } },
  { 375, { 66.10, 40.45, 4.55 } },
  { 380, { 63.40, 38.50, 3.00 } },
  { 385, { 64.60, 36.75, 2.10 } },
  { 390, { 65.80, 35.00, 1.20 } },
  { 395, { 80.30, 39.20, 0.05 } },
  { 400, { 94.80, 43.40, -1.10 } },
  { 405, { 99.80, 44.85, -0.80 } },
  { 410, { 104.80, 46.30, -0.50 } },
  { 415, { 105.35, 45.10, -0.60 } },
  { 420, { 105.90, 43.90, -0.70 } },
  { 425, { 101.35, 40.50, -0.95 } },
  { 430, { 96.80, 37.10, -1.20 } },
  { 435, { 105.35, 36.90, -1.90 } },
  { 440, { 113.90, 36.70, -2.60 } },
  { 445, { 119.75, 36.30, -2.75 } },
  { 450, { 125.60, 35.90, -2.90 } },
  { 455, { 125.55, 34.25, -2.85 } },
  { 460, { 125.50, 32.60, -2.80 } },
  { 465, { 123.40, 30.25, -2.70 } },
  { 470, { 121.30, 27.90, -2.60 } },
  { 475, { 121.30, 26.10, -2.60 } },
  { 480, { 121.30, 24.30, -2.60 } },
  { 485, { 117.40, 22.20, -2.20 } },
  { 490, { 113.50, 20.10, -1.80 } },
  { 495, { 113.30, 18.15, -1.65 } },
  { 500, { 113.10, 16.20, -1.50 } },
  { 505, { 111.95, 14.70, -1.40 } },
  { 510, { 110.80, 13.20, -1.30 } },
  { 515, { 108.65, 10.90, -1.25 } },
  { 520, { 106.50, 8.60, -1.20 } },
  { 525, { 107.65, 7.35, -1.10 } },
  { 530, { 108.80, 6.10, -1.00 } },
  { 535, { 107.05, 5.15, -0.75 } },
  { 540, { 105.30, 4.20, -0.50 } },
  { 545, { 104.85, 3.05, -0.40 } },
  { 550, { 104.40, 1.90, -0.30 } },
  { 555, { 102.20, 0.95, -0.15 } },
  { 560, { 100.00, 0.00, 0.00 } },
  { 565, { 98.00, -0.80, 0.10 } },
  { 570, { 96.00, -1.60, 0.20 } },
  { 575, { 95.55, -2.55, 0.35 } },
  { 580, { 95.10, -3.50, 0.50 } },
  { 585, { 92.10, -3.50, 1.30 } },
  { 590, { 89.10, -3.50, 2.10 } },
  { 595, { 89.80, -4.65, 2.65 } },
  { 600, { 90.50, -5.80, 3.20 } },
  { 605, { 90.40, -6.50, 3.65 } },
  { 610, { 90.30, -7.20, 4.10 } },
  { 615, { 89.35, -7.90, 4.40 } },
  { 620, { 88.40, -8.60, 4.70 } },
  { 625, { 86.20, -9.05, 4.90 } },
  { 630, { 84.00, -9.50, 5.10 } },
  { 635, { 84.55, -10.20, 5.90 } },
  { 640, { 85.10, -10.90, 6.70 } },
  { 645, { 83.50, -10.80, 7.00 } },
  { 650, { 81.90, -10.70, 7.30 } },
  { 655, { 82.25, -11.35, 7.95 } },
  { 660, { 82.60, -12.00, 8.60 } },
  { 665, { 83.75, -13.00, 9.20 } },
  { 670, { 84.90, -14.00, 9.80 } },
  { 675, { 83.10, -13.80, 10.00 } },
  { 680, { 81.30, -13.60, 10.20 } },
  { 685, { 76.60, -12.80, 9.25 } },
  { 690, { 71.90, -12.00, 8.30 } },
  { 695, { 73.10, -12.65, 8.95 } },
  { 700, { 74.30, -13.30, 9.60 } },
  { 705, { 75.35, -13.10, 9.05 } },
  { 710, { 76.40, -12.90, 8.50 } },
  { 715, { 69.85, -11.75, 7.75 } },
  { 720, { 63.30, -10.60, 7.00 } },
  { 725, { 67.50, -11.10, 7.30 } },
  { 730, { 71.70, -11.60, 7.60 } },
  { 735, { 74.35, -11.90, 7.80 } },
  { 740, { 77.00, -12.20, 8.00 } },
  { 745, { 71.10, -11.20, 7.35 } },
  { 750, { 65.20, -10.20, 6.70 } },
  { 755, { 56.45, -9.00, 5.95 } },
  { 760, { 47.70, -7.80, 5.20 } },
  { 765, { 58.15, -9.50, 6.30 } },
  { 770, { 68.60, -11.20, 7.40 } },
  { 775, { 66.80, -10.80, 7.10 } },
  { 780, { 65.00, -10.40, 6.80 } },
  { 785, { 65.50, -10.50, 6.90 } },
  { 790, { 66.00, -10.60, 7.00 } },
  { 795, { 63.50, -10.15, 6.70 } },
  { 800, { 61.00, -9.70, 6.40 } },
  { 805, { 57.15, -9.00, 5.95 } },
  { 810, { 53.30, -8.30, 5.50 } },
  { 815, { 56.10, -8.80, 5.80 } },
  { 820, { 58.90, -9.30, 6.10 } },
  { 825, { 60.40, -9.55, 6.30 } },
  { 830, { 61.90, -9.80, 6.50 } }
};

static const size_t cie_daylight_components_entry_count = sizeof(cie_daylight_components) / sizeof(_cie_std_daylight_component);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
