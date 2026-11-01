#include "table3d_interpolate.h"
#include "maths.h"
#include "unit_testing.h"
#include "table2d.h"

/**
 * @file
 *
 * @brief Support functions for 3D table interpolation.
 */

/// @name Fixed point math
/// @{

/// @brief Unsigned fixed point number type with 1 integer bit & 8 fractional bits.
/// 
/// @see https://en.wikipedia.org/wiki/Q_(number_format).
///
/// This is specialised for the number range 0..1 - a generic fixed point
/// class would miss some important optimisations. Specifically, we can avoid
/// type promotion during multiplication. 
typedef uint16_t QU1X8_t;

/** @brief Integer shift to convert to/from QU1X8_t. */
constexpr QU1X8_t QU1X8_INTEGER_SHIFT = 8;

static constexpr QU1X8_t toQU1X8(uint16_t base) {
  return base << QU1X8_INTEGER_SHIFT;
}
static constexpr uint16_t fromQU1X8(QU1X8_t base) {
  return base >> QU1X8_INTEGER_SHIFT;
}

/** @brief Precomputed value of 1 in QU1X8_t. */
TESTABLE_CONSTEXPR QU1X8_t QU1X8_ONE = toQU1X8(1U);

/** @brief Precomputed value of 0.5 in QU1X8_t. */
TESTABLE_CONSTEXPR QU1X8_t QU1X8_HALF = QU1X8_ONE/2U;

/** @brief Multiply two QU1X8_t values. */
TESTABLE_INLINE_STATIC QU1X8_t mulQU1X8(QU1X8_t a, QU1X8_t b)
{
    // 1x1 == 1....but the real reason for this is to avoid 16-bit multiplication overflow.
    //
    // We are using uint16_t as our underlying fixed point type. If we follow the regular
    // code path, we'd need to promote to uint32_t to avoid overflow.
    //
    // The overflow can only happen when *both* the X & Y inputs
    // are at the edge of a bin. 
    //
    // This is a rare condition, so most of the time we can use 16-bit multiplication and gain performance
    if (a==QU1X8_ONE && b==QU1X8_ONE)
    {
        return QU1X8_ONE;
    }
  // Add the equivalent of 0.5 to the final calculation pre-rounding.
  // This will have the effect of rounding to the nearest integer, rather
  // than always rounding down.
  return fromQU1X8((a * b) + QU1X8_HALF);
}

/// @}

/// @name Interpolation
/// @{

/**
 * @brief Compute the % position of a value within a bin.
 * 
 *  - 0%==at/below the bin minimum 
 *  - 100%==at/above the bin maximum
 *  - 50%==in the middle of the bin.
 * 
 * @note The multiplier is used to scale the axis values to the same scale as the value being checked.
 * *This retains the full precision of the axis values, thus the computed position and eventually the final interpolated result*
 * 
 * @param value The value to check.
 * @param bin The axis bin.
 * @param multiplier The multiplier for the axis values.
 * @return QU1X8_t The % position of the value within the bin.
 */
TESTABLE_INLINE_STATIC QU1X8_t compute_bin_position(const uint16_t &value, const table3d_bin_t &bin, const uint16_t &multiplier)
{
  uint16_t binMinValue = (uint16_t)bin.lowerValue()*multiplier;
  if (value<=binMinValue) { return 0U; }
  uint16_t binMaxValue = (uint16_t)bin.upperValue()*multiplier;
  if (value>=binMaxValue) { return QU1X8_ONE; }
  uint16_t binWidth = binMaxValue-binMinValue;

  // Since we can have bins of any width, we need to use 
  // 24.8 fixed point to avoid overflow
  uint16_t binPosition = value - binMinValue;
  uint32_t p = (uint32_t)binPosition << QU1X8_INTEGER_SHIFT;
  // But since we are computing the ratio (0 to 1), p is guaranteed to be
  // less than binWidth and thus the division below will result in a value
  // <=1. So we can reduce the data type from 24.8 (uint32_t) to 1.8 (uint16_t)
  return fast_div32_16(p, (uint16_t)binWidth);  
}

/** @brief Get the top right corner of the *value* coordinates in a 3D table, based on x/y axis coords. */
TESTABLE_INLINE_STATIC row_col2d toTopRight(const table3d_bin_t &xBin, const table3d_bin_t &yBin, const table3d_dim_t &axisSize)
{
  return { (table3d_dim_t)(axisSize*yBin.upperIndex), xBin.upperIndex };
}

/** @brief Get the bottom left corner of the *value* coordinates in a 3D table, based on top right corner. */
static inline row_col2d toBottomLeft(const row_col2d &topRight, const table3d_dim_t &axisSize)
{
  return { (table3d_dim_t)(topRight.row - axisSize), (table3d_dim_t)(topRight.col - UINT8_C(1)) };
}

/** @brief Combine 4 corner values with pre-computed QU1X8 bin weights.
 *
 * Shared by the uint8 and the signed int16 interpolation paths so the weighting can
 * never drift between them. The arithmetic is performed in int32_t: the widest input is
 * a signed 16-bit corner (magnitude 32768) scaled by QU1X8_ONE (256), giving 8,388,608 -
 * comfortably inside int32_t. Accumulating four of those cannot overflow either, because
 * the four weights sum to exactly QU1X8_ONE.
 *
 * @return The weighted sum, still in QU1X8 fixed point (the caller shifts it down).
 */
template <typename TValue>
static inline int32_t combine_corners(const TValue &tl,
                                      const TValue &tr,
                                      const TValue &bl,
                                      const TValue &br,
                                      const QU1X8_t &m,
                                      const QU1X8_t &n,
                                      const QU1X8_t &o,
                                      const QU1X8_t &r) {
  return ((int32_t)tl * (int32_t)m)
       + ((int32_t)tr * (int32_t)n)
       + ((int32_t)bl * (int32_t)o)
       + ((int32_t)br * (int32_t)r);
}

/** @brief Round a QU1X8-fixed value to an integer by truncation.
 *
 * The unsigned path has always used a plain truncating shift. For the non-negative values
 * the uint8 tables can hold that is "round down", and the existing interpolator test
 * expectations were written against it, so it must not change. The signed path owns the
 * symmetric rounding below.
 */
static inline table3d_value_t fromQU1X8_round_down(int32_t fixedValue) {
  return (table3d_value_t)fromQU1X8((uint32_t)fixedValue);
}

/** @brief Round a QU1X8-fixed value to the nearest integer, half away from zero.
 *
 * Symmetric about zero is required for the trim tables: a trim of exactly 0 must
 * interpolate to exactly 0 (so "no trim" is bit-exact and costs nothing), and a trim
 * cell surrounded by equal values must reproduce that value exactly.
 */
static inline int16_t fromQU1X8_round_symmetric(int32_t fixedValue) {
  if (fixedValue >= 0) {
    return (int16_t)((fixedValue + (int32_t)QU1X8_HALF) >> QU1X8_INTEGER_SHIFT);
  }
  return (int16_t)(-(((-fixedValue) + (int32_t)QU1X8_HALF) >> QU1X8_INTEGER_SHIFT));
}

/** @brief The 4 corner weights of a bilinear interpolation, in QU1X8 fixed point. */
struct bilinear_weights {
  QU1X8_t m;  ///< Weight for the top right corner
  QU1X8_t n;  ///< Weight for the bottom right corner
  QU1X8_t o;  ///< Weight for the top left corner
  QU1X8_t r;  ///< Weight for the bottom left corner
};

static inline bilinear_weights compute_bilinear_weights(const QU1X8_t &dx, const QU1X8_t &dy) {
  return {
    mulQU1X8(QU1X8_ONE-dx, dy),           // m: tr
    mulQU1X8(dx, dy),                     // n: br
    mulQU1X8(QU1X8_ONE-dx, QU1X8_ONE-dy), // o: tl
    mulQU1X8(dx, QU1X8_ONE-dy)            // r: bl
  };
}

/**
 * @brief 2d interpolation, given 4 corner values and x/y percentages
 *
 * <pre>
 *  tl----------------tr
 *  |                 |
 *  |                 |
 *  |                 |
 *  |>>>>>dx>>>>?     |
 *  |           ^     |
 *  |           dy    |
 *  |           ^     |
 *  bl----------------br
 * </pre>
 * 
 * @param tl Top left value 
 * @param tr Top right value 
 * @param bl Bottom left value 
 * @param br Bottom right value 
 * @param dx X distance
 * @param dy Y distance
 * @return table3d_value_t 
 */
TESTABLE_INLINE_STATIC table3d_value_t bilinear_interpolation( const table3d_value_t &tl,
                                                      const table3d_value_t &tr,
                                                      const table3d_value_t &bl,
                                                      const table3d_value_t &br,
                                                      const QU1X8_t &dx,
                                                      const QU1X8_t &dy) {
  const bilinear_weights w = compute_bilinear_weights(dx, dy);
  return fromQU1X8_round_down(combine_corners(tl, tr, bl, br, w.m, w.n, w.o, w.r));
}

/**
 * @brief Interpolate a table value from axis bins & values.
 * 
 * @param lookUpValues The x & y axis values we are interpolating
 * @param axisSize The length of an axis
 * @param pValues The interpolation source values
 * @param xBin The x-axis bin containing lookUpValues.x
 * @param xMultiplier The x-axis multiplier
 * @param yBin The y-axis bin containing lookUpValues.y
 * @param yMultiplier The y-axis multiplier
 * @return table3d_value_t 
 */
 table3d_value_t interpolate_3d_value(const xy_pair_t &lookUpValues, 
                    const table3d_dim_t &axisSize,
                    const table3d_value_t *pValues,
                    const table3d_bin_t &xBin,
                    const uint16_t xMultiplier,
                    const table3d_bin_t &yBin,
                    const uint16_t yMultiplier)
{
  /*
  3D Tables have an origin (0,0) in the top left hand corner. Vertical axis is expressed first.
  Eg: 2x2 table
  -----
  |2 7|
  |1 4|
  -----

  (0,1) = 7
  (0,0) = 2
  (1,0) = 1
  (1,1) = 4
  */  
  row_col2d tr = toTopRight(xBin, yBin, axisSize);
  row_col2d bl = toBottomLeft(tr, axisSize);

  /*
  At this point we have 2 corners of the map where the interpolated value will fall in
  Eg: ()          (tr.x,tr.y)

      (bl.x,bl.y) ()

  Translate those 2 corners into the actual values:
            A          B

            C          D
  Note that the values are stored in a 1D array, so we need to calculate the indices 
  appropriately based on the array layout.
  */
  table3d_value_t A = pValues[tr.row + bl.col];
  table3d_value_t B = pValues[tr.row + tr.col];
  table3d_value_t C = pValues[bl.row + bl.col];
  table3d_value_t D = pValues[bl.row + tr.col];  
  
  //Check that all values aren't just the same (This regularly happens with things like the fuel trim maps)
  if( (A == B) && (A == C) && (A == D) ) 
  { 
    return A;
  }
  else
  {
    //Create some normalised position values
    //These are essentially percentages (between 0 and 1) of where the desired value falls between the nearest bins on each axis
    const QU1X8_t p = compute_bin_position(lookUpValues.x, xBin, xMultiplier);
    const QU1X8_t q = compute_bin_position(lookUpValues.y, yBin, yMultiplier);
    return bilinear_interpolation(A, B, C, D, p, q);
  }
}

int16_t interpolate_3d_value_s16(const xy_pair_t &lookUpValues,
                    const table3d_dim_t &axisSize,
                    const int16_t *pValues,
                    const table3d_bin_t &xBin,
                    const uint16_t xMultiplier,
                    const table3d_bin_t &yBin,
                    const uint16_t yMultiplier)
{
  // Cell geometry is identical to the unsigned path - both use the same
  // row-byte layout and the same top-right / bottom-left corner derivation.
  const row_col2d tr = toTopRight(xBin, yBin, axisSize);
  const row_col2d bl = toBottomLeft(tr, axisSize);

  const int16_t A = pValues[tr.row + bl.col]; // top left
  const int16_t B = pValues[tr.row + tr.col]; // top right
  const int16_t C = pValues[bl.row + bl.col]; // bottom left
  const int16_t D = pValues[bl.row + tr.col]; // bottom right

  // Fast path: a flat cell (very common - an untouched trim map is all zeroes)
  // must return exactly the stored value, not a rounded product.
  if ((A == B) && (A == C) && (A == D))
  {
    return A;
  }

  const QU1X8_t p = compute_bin_position(lookUpValues.x, xBin, xMultiplier);
  const QU1X8_t q = compute_bin_position(lookUpValues.y, yBin, yMultiplier);

  const bilinear_weights w = compute_bilinear_weights(p, q);
  return fromQU1X8_round_symmetric(combine_corners(A, B, C, D, w.m, w.n, w.o, w.r));
}

/// @}