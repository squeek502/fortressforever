//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

//========= Copyright � 1996-2001, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================
#include "cbase.h"
#include "gamemovement.h"
#include "ff_gamerules.h"
#include "ff_shareddefs.h"
#include "in_buttons.h"
#include "movevars_shared.h"
#include "ff_mapguide.h"

#define	STOP_EPSILON		0.1
#define	MAX_CLIP_PLANES		5

#ifdef CLIENT_DLL
	#include "c_ff_player.h"
#else
	#include "ff_player.h"
#endif

// When changing jump height, recalculate the FF_MUL_CONSTANT!!!
#define FF_JUMP_HEIGHT 27.5f // Modified by Mulch 10/20/2005 so we could jump on 63 but not 64 unit high stuff
#define FF_MUL_CONSTANT 209.76177f //sqrt(2.0f * 800.0f * FF_JUMP_HEIGHT);
//static ConVar FF_JUMP_HEIGHT( "ffdev_jump_height", "27.5" );

static ConVar sv_trimpmultiplier("sv_trimpmultiplier", "1.4", FCVAR_REPLICATED);
static ConVar sv_trimpdownmultiplier("sv_trimpdownmultiplier", "1.2", FCVAR_REPLICATED);
static ConVar sv_trimpmax("sv_trimpmax", "5000", FCVAR_REPLICATED);
static ConVar sv_trimptriggerspeed("sv_trimptriggerspeed", "550", FCVAR_REPLICATED);
static ConVar sv_trimptriggerspeeddown("sv_trimptriggerspeeddown", "50", FCVAR_REPLICATED);
class CBasePlayer;

//=============================================================================
// Overrides some CGameMovement stuff
//=============================================================================
class CFFGameMovement : public CGameMovement
{
	DECLARE_CLASS(CFFGameMovement, CGameMovement);

public:
	// CGameMovement
	virtual bool CheckJumpButton();
	virtual bool CanAccelerate();
	virtual void FullNoClipMove(float factor, float maxacceleration);
	virtual void CheckVelocity( void );

	// CFFGameMovement
	virtual void FullBuildMove( void );	

	CFFGameMovement() {};
};

static ConVar bhop_cap("ffdev_bhop_cap", "1", FCVAR_REPLICATED);
static ConVar bhop_baseline("ffdev_bhop_baseline", "1", FCVAR_REPLICATED);
static ConVar bhop_pcfactor("ffdev_bhop_pcfactor", "0.65", FCVAR_REPLICATED);

//-----------------------------------------------------------------------------
// Purpose: Provides TFC jump heights, trimping, doublejumps
//-----------------------------------------------------------------------------
bool CFFGameMovement::CheckJumpButton(void)
{
	CFFPlayer *ffplayer = ToFFPlayer(player);
	if(ffplayer == NULL)
	{
		return BaseClass::CheckJumpButton();
	}

	if (player->pl.deadflag)
	{
		mv->m_nOldButtons |= IN_JUMP ;	// don't jump again until released
		return false;
	}

	// See if we are waterjumping.  If so, decrement count and return.
	if (player->m_flWaterJumpTime)
	{
		player->m_flWaterJumpTime -= gpGlobals->frametime;
		if (player->m_flWaterJumpTime < 0)
			player->m_flWaterJumpTime = 0;

		return false;
	}

	// If we are in the water most of the way...	
	if ( player->GetWaterLevel() >= 2 )
	{
		// swimming, not jumping
		SetGroundEntity( (CBaseEntity *)NULL );

		if(player->GetWaterType() == CONTENTS_WATER)    // We move up a certain amount
			mv->m_vecVelocity[2] = 100;
		else if (player->GetWaterType() == CONTENTS_SLIME)
			mv->m_vecVelocity[2] = 80;

		// play swiming sound
		if ( player->m_flSwimSoundTime <= 0 )
		{
			// Don't play sound again for 1 second
			player->m_flSwimSoundTime = 1000;
			PlaySwimSound();
		}

		return false;
	}

	// No more effect
	if (player->GetGroundEntity() == NULL)
	{
		//mv->m_nOldButtons |= IN_JUMP;

		// hack to enable Q3-style jumping
		// To jump as soon as you hit the ground, the jump button must be pressed and held in the downward part
		// of your jump only -- if you start holding it during the upward part of your previous jump, it won't work.
		// This hack just clears the current state of the jump button as long as jump was not held at the time that
		// holding the jump button became "legal" again.
		// Thus, with this hack, mv->m_nOldButtons & IN_JUMP will never be set in the air unless it has been set
		// constantly ever since the player initially jumped or unless the player has re-pressed the jump button in the
		// upwards phase of their jump.
		if(!(mv->m_nOldButtons & IN_JUMP))
		{
			/*
			if(mv->m_vecVelocity[2] < 0.0f)
			{
				mv->m_nButtons &= ~IN_JUMP;
			}
			*/

			// 06/13/2005 - Mulchman (as per Defrag)
			// Clear the jump regardless of our
			// vertical velocity - so we can
			// hit jump on the upward or downward
			// part of the jump
			mv->m_nButtons &= ~IN_JUMP;
		}
//		else
//		{
//			if(ffplayer->m_iLocalSkiState == 0)
//			{
//				ffplayer->StartSkiing();
//			}
//		}
		return false;		// in air, so no effect
	}

	if ( mv->m_nOldButtons & IN_JUMP )
	{
		return false;		// don't pogo stick
	}

	// Don't allow jumping when the player is in a stasis field.
	if ( player->m_Local.m_bSlowMovement )
		return false;

	// Cannot jump will in the unduck transition.
	//if ( player->m_Local.m_bDucking && (  player->GetFlags() & FL_DUCKING ) )
	//	return false;

	// Still updating the eye position.
	//if ( player->m_Local.m_flDuckJumpTime > 0.0f )
	//	return false;

	// UNDONE: Now that we've allowed jumps to take place while unducking, we have to
	// immediately finish the unduck. This pretty much follows TFC behaviour
	// now.
	//if (player->m_Local.m_bDucking && (player->GetFlags() & FL_DUCKING))
	//{
	//	FinishUnDuck();
	//}

	// In the air now.
	SetGroundEntity( (CBaseEntity *)NULL );

	// This following dynamic cap is documented here:
	//		http://www.madabouthats.org/code-tf2/viewtopic.php?t=2360

	const float baseline = /*1.9f*/ /*1.52f*/ bhop_baseline.GetFloat() * mv->m_flMaxSpeed;
	const float cap = /*2.0f*/ /*1.6f*/ bhop_cap.GetFloat() * mv->m_flMaxSpeed;
	const float pcfactor = /*0.5f*/ bhop_pcfactor.GetFloat();
	const float speed = FastSqrt(mv->m_vecVelocity[0] * mv->m_vecVelocity[0] + mv->m_vecVelocity[1] * mv->m_vecVelocity[1]);

	if (speed > cap)
	{
		float applied_cap = (speed - cap) * pcfactor + baseline;
		float multi = applied_cap / speed;

		mv->m_vecVelocity[0] *= multi;
		mv->m_vecVelocity[1] *= multi;

		Assert(multi <= 1.0f);
	}

	// Mirv: Play proper jump sounds
	//player->PlayStepSound( mv->m_vecAbsOrigin, player->m_pSurfaceData, 1.0, true );
	ffplayer->PlayJumpSound(mv->m_vecAbsOrigin, player->m_pSurfaceData, 1.0);

	// Mirv: This fixes the jump animation
	//MoveHelper()->PlayerSetAnimation( PLAYER_JUMP );
	ffplayer->DoAnimationEvent(PLAYERANIMEVENT_JUMP);

	float fGroundFactor = 1.0f;
	if (player->m_pSurfaceData)
	{
		fGroundFactor = player->m_pSurfaceData->game.jumpFactor; 
	}

	// --> Mirv: Trimp code v2.0!
	//float fMul = FF_MUL_CONSTANT;
	//float fMul = 268.3281573;
	// This is the base jump height before adding trimp/doublejump height
	float fMul = 268.6261342; //sqrt(2.0f * 800.0f * 45.1f);


	trace_t pm;

	// Adjusted for TFC bboxes.
	// TODO: Look at this later
	Vector vecStart = mv->m_vecAbsOrigin; // + Vector(0, 0, GetPlayerMins()[2] + 1.0f);
	Vector vecStop = vecStart - Vector(0, 0, 60.0f);
	
	TracePlayerBBox(vecStart, vecStop, MASK_PLAYERSOLID, COLLISION_GROUP_PLAYER_MOVEMENT, pm);

	// Found the floor
	if(pm.fraction != 1.0f)
	{
		// Take the lateral velocity
		Vector vecVelocity = mv->m_vecVelocity * Vector(1.0f, 1.0f, 0.0f);
		float flHorizontalSpeed = vecVelocity.Length();

		// If building, don't let them trimp!
		if( ffplayer->IsBuilding() )
			flHorizontalSpeed = 0.0f;

		if (flHorizontalSpeed > 0)
			vecVelocity /= flHorizontalSpeed;

        float flDotProduct = DotProduct(vecVelocity, pm.plane.normal);
		float flRampSlideDotProduct = DotProduct(mv->m_vecVelocity, pm.plane.normal);

		// They have to be at least moving a bit
		if (flHorizontalSpeed > sv_trimptriggerspeed.GetFloat())
		{
			// Don't do anything for flat ground or downwardly sloping (relative to motion)
			// Changed to 0.15f to make it a bit less trimpy on only slightly uneven ground
			//if (flDotProduct < -0.15f || flDotProduct > 0.15f)
			if (flDotProduct < -0.15f)
			{
				// This is one way to do it
				fMul += -flDotProduct * flHorizontalSpeed * sv_trimpmultiplier.GetFloat(); //0.6f;
				DevMsg("[S] Trimp %f! Dotproduct:%f. Horizontal speed:%f. Rampslide dot.p.:%f\n", fMul, flDotProduct, flHorizontalSpeed, flRampSlideDotProduct);
				
				if (sv_trimpmultiplier.GetFloat() > 0)
				{
					mv->m_vecVelocity[0] *= (1.0f / sv_trimpmultiplier.GetFloat() );
					mv->m_vecVelocity[1] *= (1.0f / sv_trimpmultiplier.GetFloat() );
				}
				// This is another that'll give some different height results
				// UNDONE: Reverted back to the original way for now
				//Vector reflect = mv->m_vecVelocity + (-2.0f * pm.plane.normal * DotProduct(mv->m_vecVelocity, pm.plane.normal));
				//float flSpeedAmount = clamp((flLength - 400.0f) / 800.0f, 0, 1.0f);
				//fMul += reflect.z * flSpeedAmount;
			}
		}
		// trigger downwards trimp at any speed
		if (flHorizontalSpeed > sv_trimptriggerspeeddown.GetFloat())
		{
			if (flDotProduct > 0.15f) // AfterShock: travelling downwards onto a downward ramp - give boost horizontally
			{
				// This is one way to do it
				//mv->m_vecVelocity[1] += -flDotProduct * mv->m_vecVelocity[2] * sv_trimpmultiplier.GetFloat(); //0.6f;
				//mv->m_vecVelocity[0] += -flDotProduct * mv->m_vecVelocity[2] * sv_trimpmultiplier.GetFloat(); //0.6f;
				//mv->m_vecVelocity[1] += -flDotProduct * fMul * sv_trimpmultiplier.GetFloat(); //0.6f;
				//mv->m_vecVelocity[0] += -flDotProduct * fMul * sv_trimpmultiplier.GetFloat(); //0.6f;
				mv->m_vecVelocity[1] *= sv_trimpdownmultiplier.GetFloat(); //0.6f;
				mv->m_vecVelocity[0] *= sv_trimpdownmultiplier.GetFloat(); //0.6f;
				DevMsg("[S] Down Trimp %f! Dotproduct:%f, upwards vel:%f, vel 1:%f, vel 0:%f\n", fMul, flDotProduct,mv->m_vecVelocity[2],mv->m_vecVelocity[1],mv->m_vecVelocity[0]);
				if (sv_trimpmultiplier.GetFloat() > 0)
				{
					fMul *= (1.0f / sv_trimpdownmultiplier.GetFloat() );
				}
				// This is another that'll give some different height results
				// UNDONE: Reverted back to the original way for now
				//Vector reflect = mv->m_vecVelocity + (-2.0f * pm.plane.normal * DotProduct(mv->m_vecVelocity, pm.plane.normal));
				//float flSpeedAmount = clamp((flLength - 400.0f) / 800.0f, 0, 1.0f);
				//fMul += reflect.z * flSpeedAmount;
			}
		}
	}
	// <-- Mirv: Trimp code v2.0!

	//// Acclerate upward
	//// If we are ducking...
	//float startz = mv->m_vecVelocity[2];
	//if ((player->m_Local.m_bDucking ) || (player->GetFlags() & FL_DUCKING))
	//{
	//	// d = 0.5 * g * t^2		- distance traveled with linear accel
	//	// t = sqrt(2.0 * 45 / g)	- how long to fall 45 units
	//	// v = g * t				- velocity at the end (just invert it to jump up that high)
	//	// v = g * sqrt(2.0 * 45 / g )
	//	// v^2 = g * g * 2.0 * 45 / g
	//	// v = sqrt( g * 2.0 * 45 )
	//	mv->m_vecVelocity[2] = fGroundFactor * fMul;  // 2 * gravity * height
	//}
	//else
	//{
	//	//mv->m_vecVelocity[2] += flGroundFactor * flMul;  // 2 * gravity * height
	//	mv->m_vecVelocity[2] = fGroundFactor * fMul;  // 2 * gravity * height
	//}

	// Double jump - but don't allow double jumps while building, please!
	if( ffplayer->m_bCanDoubleJump && !ffplayer->IsBuilding() )
	{
		float flElapsed = ffplayer->m_flNextJumpTimeForDouble - gpGlobals->curtime;

		if (flElapsed > 0 && flElapsed < 0.4f)
		{
			// AfterShock: Add a set amount for a double jump (dont multiply)
			fMul += 190.0f;

#ifdef GAME_DLL
			DevMsg("[S] Double jump %f!\n", fMul);
#else
			//DevMsg("[C] Double jump %f!\n", fMul);
#endif

			ffplayer->m_bCanDoubleJump = false;
		}

		ffplayer->m_flNextJumpTimeForDouble = gpGlobals->curtime + 0.5f;
	}

	// --> Mirv: Add on new velocity
	if( mv->m_vecVelocity[2] < 0 )
		mv->m_vecVelocity[2] = 0;
//	if (fMul > 500)
//		DevMsg("[S] vert velocity_old %f!\n", mv->m_vecVelocity[2]);
		
	// This is commented out because trimp code was getting called twice and doubling the effect sometimes.
	//mv->m_vecVelocity[2] += fMul;
	mv->m_vecVelocity[2] = fMul;

//	if (fMul > 500)
//		DevMsg("[S] vert velocity %f!\n", mv->m_vecVelocity[2]);
				
	mv->m_vecVelocity[2] = min(mv->m_vecVelocity[2], sv_trimpmax.GetFloat());
//	if (fMul > 500)
//		DevMsg("[S] vert velocity2 %f!\n", mv->m_vecVelocity[2]);
	
	// <-- Mirv: Add on new velocity

	FinishGravity();

//	mv->m_outJumpVel.z += mv->m_vecVelocity[2] - startz;
//	mv->m_outStepHeight += 0.15f;

	// Set jump time.
/*
	if ( gpGlobals->maxClients == 1 )
	{
		player->m_Local.m_flJumpTime = GAMEMOVEMENT_JUMP_TIME;
		player->m_Local.m_bInDuckJump = true;
	}
*/

	// Flag that we jumped.
	mv->m_nOldButtons |= IN_JUMP;	// don't jump again until released
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Mapguide movement. Needs sorting out at some point
//-----------------------------------------------------------------------------
void CFFGameMovement::FullNoClipMove(float factor, float maxacceleration)
{
#ifdef CLIENT_DLL
	//CFFPlayer *ffplayer = (CFFPlayer *) player;

	//if( ffplayer->m_flNextMapGuideTime > gpGlobals->curtime )
	//{
	//	CFFMapGuide *nextguide = ffplayer->m_hNextMapGuide;
	//	CFFMapGuide *lastguide = ffplayer->m_hLastMapGuide;

	//	float t = clamp( ( ffplayer->m_flNextMapGuideTime - gpGlobals->curtime ) / 10.0f, 0, 1.0f );

	//	Vector vecNewPos = t * lastguide->GetAbsOrigin() + ( 1 - t ) * nextguide->GetAbsOrigin();
	//	QAngle angNewDirection = t * lastguide->GetAbsAngles() + ( 1 - t ) * nextguide->GetAbsAngles();

	//	// Apply these here
	//}
	//else
#endif
		BaseClass::FullNoClipMove(factor, maxacceleration);
}

//-----------------------------------------------------------------------------
// Purpose: Movement while building in Fortress Forever
//-----------------------------------------------------------------------------
void CFFGameMovement::FullBuildMove( void )
{
	CFFPlayer *pPlayer = ToFFPlayer( player );
	if( !pPlayer )
		return;

	// Don't care if dead or not building...
	if( !pPlayer->IsBuilding() || !pPlayer->IsAlive() )
		return;
	
	// Don't care if under water...
	if( pPlayer->GetWaterLevel() > WL_Feet )
		return;

	// Finally, allow jumping

	StartGravity();

	// Was jump button pressed?
	if( mv->m_nButtons & IN_JUMP )
	{
		CheckJumpButton();
	}
	else
	{
		mv->m_nOldButtons &= ~IN_JUMP;
	}

	// Reset these so we stay in place
	mv->m_flSideMove = 0.0f;
	mv->m_flForwardMove = 0.0f;
	mv->m_vecVelocity[ 0 ] = 0.0f;
	mv->m_vecVelocity[ 1 ] = 0.0f;

	CheckVelocity();

	if( pPlayer->GetGroundEntity() != NULL )
	{
		WalkMove();
	}
	else
	{
		AirMove();  // Take into account movement when in air.
	}

	CategorizePosition();

	FinishGravity();

	// Does nothing, but makes me feel happy.
	BaseClass::FullBuildMove();
}

//-----------------------------------------------------------------------------
// Purpose: Allow spectators to move
//-----------------------------------------------------------------------------
bool CFFGameMovement::CanAccelerate()
{
	// Observers can accelerate
	if (player->IsObserver())
		return true;

	// Dead players don't accelerate.
	if (player->pl.deadflag)
		return false;

	// If waterjumping, don't accelerate
	if (player->m_flWaterJumpTime)
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Check player velocity & clamp if cloaked
//-----------------------------------------------------------------------------
void CFFGameMovement::CheckVelocity( void )
{
	// Let regular movement clamp us within movement limits
	BaseClass::CheckVelocity();

	// Clamp further only if we're cloaked
	
	CFFPlayer *pPlayer = ToFFPlayer( player );
	if( !pPlayer )
		return;
	
	if( !pPlayer->IsCloaked() )
		return;

	float flMaxCloakSpeed = ffdev_spy_maxcloakspeed.GetFloat();

	// Going over speed limit, need to clamp so we don't uncloak
	if( mv->m_vecVelocity.LengthSqr() > ( flMaxCloakSpeed * flMaxCloakSpeed ) )
	{
		mv->m_vecVelocity.x *= 0.5f;
		mv->m_vecVelocity.y *= 0.5f;
	}
}

// Expose our interface.
static CFFGameMovement g_GameMovement;
IGameMovement *g_pGameMovement = ( IGameMovement * )&g_GameMovement;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CFFGameMovement, IGameMovement,INTERFACENAME_GAMEMOVEMENT, g_GameMovement );
