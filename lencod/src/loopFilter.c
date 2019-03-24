/*
***********************************************************************
* COPYRIGHT AND WARRANTY INFORMATION
*
* Copyright 2001, International Telecommunications Union, Geneva
*
* DISCLAIMER OF WARRANTY
*
* These software programs are available to the user without any
* license fee or royalty on an "as is" basis. The ITU disclaims
* any and all warranties, whether express, implied, or
* statutory, including any implied warranties of merchantability
* or of fitness for a particular purpose.  In no event shall the
* contributor or the ITU be liable for any incidental, punitive, or
* consequential damages of any kind whatsoever arising from the
* use of these programs.
*
* This disclaimer of warranty extends to the user of these programs
* and user's customers, employees, agents, transferees, successors,
* and assigns.
*
* The ITU does not represent or warrant that the programs furnished
* hereunder are free of infringement of any third-party patents.
* Commercial implementations of ITU-T Recommendations, including
* shareware, may be subject to royalty fees to patent holders.
* Information regarding the ITU-T patent policy is available from
* the ITU Web site at http://www.itu.int.
*
* THIS IS NOT A GRANT OF PATENT RIGHTS - SEE THE ITU-T PATENT POLICY.
************************************************************************
*/

/*!
 *************************************************************************************
 * \file loopFilter.c
 *
 * \brief
 *    Filter to reduce blocking artifacts on a macroblock level.
 *    The filter strengh is QP dependent.
 *
 * \author
 *    Contributors:
 *    - Peter List      Peter.List@t-systems.de:  Original code                                               (13-Aug-2001)
 *    - Jani Lainema    Jani.Lainema@nokia.com:   Some bug fixing, removal of recusiveness                    (16-Aug-2001)
 *    - Peter List      Peter.List@t-systems.de:  recusiveness back and various simplifications               (10-Jan-2002)
 *    - Peter List      Peter.List@t-systems.de:  New structure, lot's of simplifications. Adopted thresholds 
                                                  for extended QPs. No more recusiveness for strog filtering, (12-Mar-2002)
 *
 *************************************************************************************
 */

#include <stdlib.h>
#include <string.h>
#include "global.h"

#define  IClip( Min, Max, Val) (((Val)<(Min))? (Min):(((Val)>(Max))? (Max):(Val)))
extern const int QP_SCALE_CR[40] ;


/*************************************************************************************/


// NOTE: to change the tables below for instance when the QP doubling is changed from 6 to 8 values 
//       send an e-mail to Peter.List@t-systems.com to get a little programm that calculates them automatically 

byte ALPHA_TABLE[40]  = {0,0,0,0,0,0,0,0,  4,4,5,6,7,9,10,12,  14,17,20,24,28,33,39,46,  55,65,76,90,106,126,148,175,  207,245,255,255,255,255,255,255} ;
byte  BETA_TABLE[40]  = {0,0,0,0,0,0,0,0,  3,3,3,4,4,4, 6, 6,   7, 7, 8, 8, 9, 9,10,10,  11,11,12,12, 13, 13, 14, 14,   15, 15, 16, 16, 17, 17, 18, 18} ;
byte CLIP_TAB[40][5]  =
 {{ 0, 0, 0, 0, 0},{ 0, 0, 0, 0, 0},{ 0, 0, 0, 0, 0},{ 0, 0, 0, 0, 0},{ 0, 0, 0, 0, 0},{ 0, 0, 0, 1, 1},{ 0, 0, 0, 1, 1},{ 0, 0, 0, 1, 1},
  { 0, 0, 0, 1, 1},{ 0, 0, 1, 1, 1},{ 0, 0, 1, 1, 1},{ 0, 1, 1, 1, 1},{ 0, 1, 1, 1, 1},{ 0, 1, 1, 1, 1},{ 0, 1, 1, 1, 1},{ 0, 1, 1, 2, 2},
  { 0, 1, 1, 2, 2},{ 0, 1, 1, 2, 2},{ 0, 1, 1, 2, 2},{ 0, 1, 2, 3, 3},{ 0, 1, 2, 3, 3},{ 0, 2, 2, 3, 3},{ 0, 2, 2, 4, 4},{ 0, 2, 3, 4, 4},
  { 0, 2, 3, 4, 4},{ 0, 3, 3, 5, 5},{ 0, 3, 4, 6, 6},{ 0, 3, 4, 6, 6},{ 0, 4, 5, 7, 7},{ 0, 4, 5, 8, 8},{ 0, 4, 6, 9, 9},{ 0, 5, 7,10,10},
  { 0, 6, 8,11,11},{ 0, 6, 8,13,13},{ 0, 7,10,14,14},{ 0, 8,11,16,16},{ 0, 9,12,18,18},{ 0,10,13,20,20},{ 0,11,15,23,23},{ 0,13,17,25,25}} ;

void GetStrength( byte Strength[4], Macroblock* MbP, Macroblock* MbQ, int dir, int edge, int blk_y, int blk_x ) ;
void EdgeLoop( byte* SrcPtr, byte Strength[4], int QP, int dir, int width, int Chro ) ;
void DeblockMb(ImageParameters *img, byte **imgY, byte ***imgUV, int blk_y, int blk_x) ;

/*!
 *****************************************************************************************
 * \brief
 *    The main Deblocking function
 *****************************************************************************************
 */

void DeblockFrame(ImageParameters *img, byte **imgY, byte ***imgUV)
  {
  int       mb_x, mb_y ;

  for( mb_y=0 ; mb_y<(img->height>>4) ; mb_y++ )
    for( mb_x=0 ; mb_x<(img->width>>4) ; mb_x++ )
      DeblockMb( img, imgY, imgUV, mb_y, mb_x ) ;
  } 


  /*!
 *****************************************************************************************
 * \brief
 *    Deblocks one macroblock
 *****************************************************************************************
 */

void DeblockMb(ImageParameters *img, byte **imgY, byte ***imgUV, int mb_y, int mb_x)
  {
  int           EdgeCondition;
  int           dir, edge, QP ;                                                          
  byte          Strength[4], *SrcY, *SrcU, *SrcV ;
  Macroblock    *MbP, *MbQ ; 
  

  SrcY = imgY    [mb_y<<4] + (mb_x<<4) ;    // pointers to source
  if (imgUV != NULL)
  {
    SrcU = imgUV[0][mb_y<<3] + (mb_x<<3) ;
    SrcV = imgUV[1][mb_y<<3] + (mb_x<<3) ;
  }
  MbQ  = &img->mb_data[mb_y*(img->width>>4) + mb_x] ;                                                 // current Mb

  for( dir=0 ; dir<2 ; dir++ )                                             // vertical edges, than horicontal edges
    {
    EdgeCondition = (dir && mb_y) || (!dir && mb_x)  ;                    // can not filter beyond frame boundaries
    for( edge=0 ; edge<4 ; edge++ )                                            // first 4 vertical strips of 16 pel
      {                                                                                       // then  4 horicontal
      if( edge || EdgeCondition )
        {
        MbP = (edge)? MbQ : ((dir)? (MbQ -(img->width>>4))  : (MbQ-1) ) ;       // MbP = Mb of the remote 4x4 block
        QP = max( 0, (MbP->qp + MbQ->qp ) >> 1) ;                                   // Average QP of the two blocks
        GetStrength( Strength, MbP, MbQ, dir, edge, mb_y<<2, mb_x<<2 ) ; //Strength for 4 pairs of blks in 1 stripe
        if( *((int*)Strength) )  // && (QP>= 8) )                    // only if one of the 4 Strength bytes is != 0
          {
          EdgeLoop( SrcY + (edge<<2)* ((dir)? img->width:1 ), Strength, QP, dir, img->width, 0 ) ; 
          if( (imgUV != NULL) && !(edge & 1) )
            {
            EdgeLoop( SrcU +  (edge<<1) * ((dir)? img->width_cr:1 ), Strength, QP_SCALE_CR[QP], dir, img->width_cr, 1 ) ; 
            EdgeLoop( SrcV +  (edge<<1) * ((dir)? img->width_cr:1 ), Strength, QP_SCALE_CR[QP], dir, img->width_cr, 1 ) ; 
            } ;
          } ;
        } ; 
      } ;
    } ;
  }




  /*!
 *********************************************************************************************
 * \brief
 *    returns a buffer of 4 Strength values for one stripe in a mb (for different Frame types)
 *********************************************************************************************
 */

int  ININT_STRENGTH[4] = {0x04040404, 0x03030303, 0x03030303, 0x03030303} ; 
byte BLK_NUM [2][4][4] = {{{0,4,8,12},{1,5,9,13},{2,6,10,14},{3,7,11,15}},   {{0,1,2,3},{4,5,6,7},{8,9,10,11},{12,13,14,15}}} ;
byte BLK_4_TO_8[16]    = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3} ;

void  GetStrength( byte Strength[4], Macroblock* MbP, Macroblock* MbQ, int dir, int edge, int block_y, int block_x ) 
  {
  int    blkP, blkQ, idx ;
  int    blk_x2, blk_y2, blk_x, blk_y ;
                                            
  *((int*)Strength) = ININT_STRENGTH[edge] ;               // Assume INTRA -> Strength=3. (or Strength=4 for Mb-edge)

  for( idx=0 ; idx<4 ; idx++ )
    {                                                                                     // if not intra or SP-frame
    blkQ = BLK_NUM[dir][ edge       ][idx] ;                 // if one of the 4x4 blocks has coefs.    set Strength=2
    blkP = BLK_NUM[dir][(edge-1) & 3][idx] ; 
    if(    !(MbP->b8mode[ BLK_4_TO_8[blkP] ]==IBLOCK || MbP->mb_type==I16MB)  && (img->types != SP_IMG)
        && !(MbQ->b8mode[ BLK_4_TO_8[blkQ] ]==IBLOCK || MbQ->mb_type==I16MB) )
      {
      if( ((MbQ->cbp_blk &  (1 << blkQ )) != 0) || ((MbP->cbp_blk &  (1 << blkP)) != 0) )
        Strength[idx] = 2 ;
      else
        {                                                   // if no coefs, but vector difference >= 1 set Strength=1 
        blk_y  = block_y + (blkQ >> 2) ;   blk_y2 = blk_y -  dir ;
        blk_x  = block_x + (blkQ  & 3)+4 ; blk_x2 = blk_x - !dir ;
        
        if( img->type == B_IMG )
          {
          if( (MbQ->mb_type==0) || (MbQ->mb_type==P8x8 && MbQ->b8mode[ BLK_4_TO_8[blkQ] ]==0) )     // if Direct mode 
            Strength[idx] = (abs(     dfMV[0][blk_y][blk_x] -     dfMV[0][blk_y2][blk_x2]) >= 4) |
                            (abs(     dfMV[1][blk_y][blk_x] -     dfMV[1][blk_y2][blk_x2]) >= 4) |
                            (abs(     dbMV[0][blk_y][blk_x] -     dbMV[0][blk_y2][blk_x2]) >= 4) |
                            (abs(     dbMV[1][blk_y][blk_x] -     dbMV[1][blk_y2][blk_x2]) >= 4)  ;
          else
            Strength[idx] = (abs( tmp_fwMV[0][blk_y][blk_x] - tmp_fwMV[0][blk_y2][blk_x2]) >= 4) |
                            (abs( tmp_fwMV[1][blk_y][blk_x] - tmp_fwMV[1][blk_y2][blk_x2]) >= 4) |
                            (abs( tmp_bwMV[0][blk_y][blk_x] - tmp_bwMV[0][blk_y2][blk_x2]) >= 4) |
                            (abs( tmp_bwMV[1][blk_y][blk_x] - tmp_bwMV[1][blk_y2][blk_x2]) >= 4)  ;
          }
        else
          Strength[idx]  =  (abs(   tmp_mv[0][blk_y][blk_x] -   tmp_mv[0][blk_y2][blk_x2]) >= 4 ) |
                            (abs(   tmp_mv[1][blk_y][blk_x] -   tmp_mv[1][blk_y2][blk_x2]) >= 4 ) |
                            (     refFrArr [blk_y][blk_x-4] !=   refFrArr[blk_y2][blk_x2-4]);

        } ;
      }  ;
    } ;
  }


/*!
 *****************************************************************************************
 * \brief
 *    Filters one edge of 16 (luma) or 8 (chroma) pel
 *****************************************************************************************
 */
void EdgeLoop( byte* SrcPtr, byte Strength[4], int QP, int dir, int width, int Chro )  
  {
  int      pel, ap, aq, PtrInc, Strng ;
  int      inc, inc2, inc3, inc4 ;
  int      C0, c0, Delta, dif, AbsDelta ;
  int      L2, L1, L0, R0, R1, R2, RL0 ;
  int      Alpha, Beta  ;
  byte*    ClipTab ;   

  PtrInc  = dir?      1 : width ;
  inc     = dir?  width : 1 ;                     // vertical filtering increment to next pixel is 1 else width
  inc2    = inc<<1 ;    
  inc3    = inc + inc2 ;    
  inc4    = inc<<2 ;
  Alpha   = ALPHA_TABLE[ QP ] ;
  Beta    = BETA_TABLE [ QP ] ;  
  ClipTab = CLIP_TAB   [ QP ] ;

  for( pel=0 ; pel<16 ; pel++ )
    {
    if( Strng = Strength[pel >> 2] )
      {
      L0  = SrcPtr [-inc ] ;
      R0  = SrcPtr [    0] ;
      AbsDelta  = abs( Delta = R0 - L0 )  ;

      if( AbsDelta < Alpha )
        {
        C0  = ClipTab[ Strng ] ;
        L1  = SrcPtr[-inc2] ;
        R1  = SrcPtr[ inc ] ;
        if( ((abs(R0 - R1) - Beta)  & (abs(L0 - L1) - Beta) ) < 0  ) 
          {
          L2  = SrcPtr[-inc3] ;
          R2  = SrcPtr[ inc2] ;
          aq  = (abs( R0 - R2) - Beta ) < 0  ;
          ap  = (abs( L0 - L2) - Beta ) < 0  ;

          if( (Strng == 4) && (ap+aq == 2) && (AbsDelta >= 2) && (AbsDelta < (QP>>2)) )    // INTRA strong filtering
            {
            RL0             = L0 + R0 ;
            SrcPtr[   0 ]   = ( L1 + ((R1 + RL0) << 1) +  SrcPtr[ inc2] + 4) >> 3 ;
            SrcPtr[-inc ]   = ( R1 + ((L1 + RL0) << 1) +  SrcPtr[-inc3] + 4) >> 3 ;

            SrcPtr[ inc ]   = ( SrcPtr[ inc3] + ((SrcPtr[ inc2] + R0 + R1) << 1) + L0 + 4) >> 3 ;
            SrcPtr[-inc2]   = ( SrcPtr[-inc4] + ((SrcPtr[-inc3] + L1 + L0) << 1) + R0 + 4) >> 3 ;
            if( !Chro )                                                                 
              {
              SrcPtr[-inc3] = (((SrcPtr[-inc4] + SrcPtr[-inc3]) <<1) + SrcPtr[-inc3] + L1 + RL0 + 4) >> 3 ;
              SrcPtr[ inc2] = (((SrcPtr[ inc3] + SrcPtr[ inc2]) <<1) + SrcPtr[ inc2] + R1 + RL0 + 4) >> 3 ;
              }
            }
          else                                                                                   // normal filtering
            {
            c0               = C0 + ap + aq ;
            dif              = IClip( -c0, c0, ( (Delta << 2) + (L1 - R1) + 4) >> 3 ) ;
            SrcPtr[  -inc ]  = IClip(0, 255, L0 + dif) ;
            SrcPtr[     0 ]  = IClip(0, 255, R0 - dif) ;

            if( !Chro )
              {
              if( ap )
                SrcPtr[-inc2] += IClip( -C0,  C0, ( L2 + SrcPtr[-inc] - (L1<<1)) >> 1 ) ;
              if( aq  )
                SrcPtr[  inc] += IClip( -C0,  C0, ( R2 + SrcPtr[   0] - (R1<<1)) >> 1 ) ;
              } ;
            } ;
          } ; 
        } ;
      SrcPtr += PtrInc ;      // Increment to next set of pixel
      pel    += Chro ;
      } 
    else
      {
      SrcPtr += PtrInc << (2 - Chro) ;
      pel    += 3 ;
      }  ;
    }
  } 