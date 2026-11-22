// ============================================================================
// osgS57/s57chart.h -- S-57 (ENC) semantic helpers on top of the ISO8211
// reader.  Kept deliberately small for stage-1 (parse + report).  The model
// will grow into OSG geometry building blocks in later stages.
// ============================================================================
#pragma once

#include "iso8211.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osgS57 {
namespace S57 {

// DSPM -- data set parameter values that drive geometry decoding.
struct DatasetParameters {
    bool present = false;

    long long rcnm = 0;
    long long rcid = 0;
    long long hdat = 0;  // horizontal datum
    long long vdat = 0;  // vertical datum
    long long sdat = 0;  // sounding datum
    long long cscl = 0;  // compilation scale
    long long duni = 0;  // units of depth (1=m 2=ft 3=fath)
    long long huni = 0;  // units of height (1=m 2=ft)
    long long puni = 0;  // units of position (1=deg 2=?)
    long long coun = 0;  // country / producing agency
    long long comf = 0;  // coordinate multiplication factor
    long long somf = 0;  // sounding multiplication factor
    std::string comt;

    // Convert a stored integer coordinate into degrees (lon/lat).
    double positionScale() const {
        // S-57 default COMF is 1,000,000 when the DSPM field is absent.
        return comf > 0 ? static_cast<double>(comf) : 1000000.0;
    }
    // Convert a stored depth integer into the sounding unit.
    double soundingScale() const {
        return somf > 0 ? static_cast<double>(somf) : 1.0;
    }

    // Multiplier that converts a value expressed in DUNI units into metres.
    // DUNI: 1=metres, 2=feet, 3=fathoms (anything else assumed metres).
    double depthUnitMetres() const {
        if (duni == 2) return 0.3048;
        if (duni == 3) return 1.8288;
        return 1.0;
    }
    // Multiplier that converts a value expressed in HUNI units into metres.
    // HUNI: 1=metres, 2=feet (anything else assumed metres).
    double heightUnitMetres() const {
        return huni == 2 ? 0.3048 : 1.0;
    }
};

// Decode the DSPM field of a data-set parameter record.
DatasetParameters readDspm(const I8211::Iso8211Reader& rdr,
                           const I8211::RecordDef& rec);

// A decoded coordinate triple from an SG2D / SG3D field.
struct ChartPoint {
    double lon = 0.0;    // degrees
    double lat = 0.0;    // degrees
    double depth = 0.0;  // in sounding units, 0 when not a 3D point
    bool hasDepth = false;
    bool valid = false;
};

// Extract the repeated coordinate triples of one spatial record
// (fields SG2D / SG3D, whichever exists).
std::vector<ChartPoint> spatialCoordinates(
    const I8211::Iso8211Reader& rdr, const I8211::RecordDef& rec,
    const DatasetParameters& dspm);

// ATTF / NATF attribute instance { code, raw value }.
struct AttributeInstance {
    long long code = 0;
    std::string value;
};

// Attributes of a feature record (repeated ATTF + NATF fields).
std::vector<AttributeInstance> readAttributes(
    const I8211::Iso8211Reader& rdr, const I8211::RecordDef& rec);

// Attribute value classification ---------------------------------------------
// ATTF/NATF values are stored as raw text ("ATVL"); the S-57 attribute
// catalogue decides the intended type.  Since the bundled catalogue carries
// no format column, values are classified syntactically: every attribute
// whose text parses fully as a number is exposed as a number, everything
// else stays text (names, dates, enumeration names, lists ...).
enum class AttrType { Text, Integer, Real };

AttrType classifyAttribute(const std::string& value);

// Convenience: parse an integer or real attribute value.  Returns false for
// text values (and for empty strings -- S-57 uses "" for "not given").
bool attributeAsDouble(const std::string& value, double& out);

// Coordinate-system helpers ---------------------------------------------------
// Wrap a longitude *difference* into (-180, +180].  Subtracting wrapped
// differences keeps cells that cross the +/-180 deg antimeridian (Fiji,
// Chukotka, western Aleutians ...) contiguous in local projections.
double wrapLon180(double lonDiff);

// Horizontal datum conversion.  S-57 Appendix A: HDAT=2 means WGS-84 and
// the ENC UOC *requires* that value; any other datum must be converted to
// WGS-84.  The parameters come from the M_HOPA meta object attributes:
//   DXDA/DYDA/DZDA  geocentric datum shift in metres (3-parameter shift)
//   ESMA            semi-major axis of the local ellipsoid (metres)
//   ESMI            inverse flattening of the local ellipsoid
struct MolodenskyParams
{
    bool identity = true;   // true: HDAT==2 or no shift parameters given
    bool known = true;      // false: HDAT code not in the built-in table
    double dx = 0.0, dy = 0.0, dz = 0.0;
    double a = 6378137.0;           // local ellipsoid semi-major axis
    double rf = 298.2572235630;     // local ellipsoid 1 / flattening
};

// Build conversion parameters for a DSPM.HDAT value.  Returns an identity
// transform for HDAT=2 (WGS-84).  HDAT codes outside the built-in table
// keep identity too, but 'known' is set to false so callers can warn.
MolodenskyParams molodenskyForHdat(long long hdat);

// Geodetic longitude/latitude (degrees, local datum) -> WGS-84 degrees.
// Height is not used (ENC vectors carry no ellipsoidal height); the
// three-parameter shift is computed at h=0, accurate to well below a metre
// for the datum sets actually shipped in ENCs.
void molodenskyToWGS84(const MolodenskyParams& p,
                       double lonDeg, double latDeg,
                       double& lonOutDeg, double& latOutDeg);

// Classification helpers -----------------------------------------------------
// RCNM codes seen in the general exchange and spatial/feature records.
enum Rcnm {
    RCNM_DSID = 10,
    RCNM_CATD = 15,
    RCNM_DSPM = 20,
    RCNM_FEATURE = 100,
    RCNM_ISOLATED_NODE = 110,
    RCNM_CONNECTED_NODE = 120,
    RCNM_EDGE = 130,
    RCNM_FACE = 140,
};

// First RCNM field (in field tag order) of the record, if any.
int recordRcnm(const I8211::Iso8211Reader& rdr,
               const I8211::RecordDef& rec);

}  // namespace S57
}  // namespace osgS57
