#pragma once
#include <vector>
#include "vector.h"

// Chell's own arms in VR (experiment): after the game has built the player's bones, the arms
// reach from shoulders below the head to the controllers, everything else is hidden.
namespace Arms
{
    // A frame in the world: origin and axes (forward, left, up).
    struct Frame
    {
        Vector origin, forward, left, up;
    };

    struct Input
    {
        Vector head; // the HMD's position
        Vector eyes; // between the eyes: hidden bones shrink to this point
        float headYaw; // degrees
        float unitsPerMeter;
        float bodyHeight; // the player's, cm: arms, hands and shoulders grow with it
        Frame aim; // the command's aim: the left arm mirrors the right one across it (origin unused)
        Frame gunHand; // where the gun's grip is, axes of the gun hand's controller
        Frame otherHand; // the mirror image of that for the other controller
        bool gunInRightHand; // Chell's right arm holds the gun
    };

    // bones: the player's bone matrices (world), as many as studioHdr (studiohdr_t) has.
    // Returns false if the model doesn't have Chell's bones.
    bool Apply(const void *studioHdr, matrix3x4_t *bones, const Input &input);

    // False if the last model given to Apply doesn't have Chell's bones.
    bool Usable();

    // Points of Chell's hand (0 = right, 1 = left) in bones (count of them, as given to Apply):
    // the wrist first, then the fingers' joints and tips; added to points. None without her bones.
    void HandPoints(const matrix3x4_t *bones, size_t count, int side, std::vector<Vector> &points);
}
