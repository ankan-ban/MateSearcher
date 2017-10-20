#include <stdio.h>
#include "chess.h"

#define SKIP_CUDA_CODE
#include "MoveGeneratorBitboard.h"

#define INF 1000

// helper routines for CPU perft
uint32 countMoves(HexaBitBoardPosition *pos, bool *mate)
{
    uint32 nMoves;
    int chance = pos->chance;

#if USE_TEMPLATE_CHANCE_OPT == 1
    if (chance == BLACK)
    {
        nMoves = MoveGeneratorBitboard::countMoves<BLACK>(pos, mate);
    }
    else
    {
        nMoves = MoveGeneratorBitboard::countMoves<WHITE>(pos, mate);
    }
#else
    nMoves = MoveGeneratorBitboard::countMoves(pos, chance);
#endif
    return nMoves;
}

uint32 generateBoards(HexaBitBoardPosition *pos, HexaBitBoardPosition *newPositions)
{
    uint32 nMoves;
    int chance = pos->chance;
#if USE_TEMPLATE_CHANCE_OPT == 1
    if (chance == BLACK)
    {
        nMoves = MoveGeneratorBitboard::generateBoards<BLACK>(pos, newPositions);
    }
    else
    {
        nMoves = MoveGeneratorBitboard::generateBoards<WHITE>(pos, newPositions);
    }
#else
    nMoves = MoveGeneratorBitboard::generateBoards(pos, newPositions, chance);
#endif

    return nMoves;
}

int isCheckMate(HexaBitBoardPosition *pos)
{
    int chance = pos->chance;
#if USE_TEMPLATE_CHANCE_OPT == 1
    if (chance == BLACK)
    {
        return MoveGeneratorBitboard::isCheckMate<BLACK>(pos);
    }
    else
    {
        return MoveGeneratorBitboard::isCheckMate<WHITE>(pos);
    }
#else
    return MoveGeneratorBitboard::isCheckMate(pos, chance);
#endif
}


uint32 generateMoves(HexaBitBoardPosition *pos, CMove *moves)
{
    uint32 nMoves;
    int chance = pos->chance;
#if USE_TEMPLATE_CHANCE_OPT == 1
    if (chance == BLACK)
    {
        nMoves = MoveGeneratorBitboard::generateMoves<BLACK>(pos, moves);
    }
    else
    {
        nMoves = MoveGeneratorBitboard::generateMoves<WHITE>(pos, moves);
    }
#else
    nMoves = MoveGeneratorBitboard::generateMoves(pos, moves, chance);
#endif

    return nMoves;
}


#define TT_BITS 26

#define SCORE_EXACT    0
#define SCORE_GE       1
#define SCORE_LE       2

union TTEntryMate
{
    uint64 hash;            // 24 LSBs overlapped with other info
    struct
    {
        int16 score;
        uint8 scoreType;
        uint8 padding[5];   // the hash part
    };
};

CT_ASSERT(sizeof(TTEntryMate) == 8);

TTEntryMate *transTable;
void allocTT()
{
    uint64 ttSize = GET_TT_SIZE_FROM_BITS(TT_BITS) * sizeof(uint64);
    transTable = (TTEntryMate *)malloc(ttSize);
    memset(transTable, 0, ttSize);
}

void freeTT()
{
    free(transTable);
}

bool lookupTT(uint64 hash, int16 *score, uint8 *scoreType)
{
    const uint64 indexBits = GET_TT_INDEX_BITS(TT_BITS);
    const uint64 hashBits  = GET_TT_HASH_BITS(TT_BITS);
    TTEntryMate hashEntry = transTable[hash & indexBits];
    
    if ((hashEntry.hash & hashBits) == (hash & hashBits))
    {
        // hash hit
        *score     = hashEntry.score;
        *scoreType = hashEntry.scoreType;
        return true;
    }

    return false;
}

void storeTT(uint64 hash, int16 score, uint8 scoreType)
{
    TTEntryMate hashEntry = {};
    hashEntry.hash = hash;
    hashEntry.score = score;
    hashEntry.scoreType = scoreType;
    const uint64 indexBits = GET_TT_INDEX_BITS(TT_BITS);
    transTable[hash & indexBits] = hashEntry;
}

uint64 gInteriorNodesVisited = 0;
uint64 gLeafNodesVisited = 0;
uint64 gNodesMate = 0;

// alpha beta pruning
int alphabeta(HexaBitBoardPosition *node, int depth, int alpha, int beta, int *bestChild = NULL)
{
    if (depth == 0)
    {
        gLeafNodesVisited++;

        if (isCheckMate(node))
        {
            gNodesMate++;
            return -1;
        }
        else
        {
            return 0;
        }
    }


#if 1
    // check hash table
    uint64 hash = MoveGeneratorBitboard::computeZobristKey(node);
    hash ^= (ZOB_KEY(depth) * depth);

    uint8 scoreType;
    int16 ttScore;
    if (lookupTT(hash, &ttScore, &scoreType))
    {
        if (scoreType == SCORE_EXACT)
            return ttScore;
        if (scoreType == SCORE_GE && ttScore >= beta)
            return ttScore;
        if (scoreType == SCORE_LE && ttScore <= alpha)
            return ttScore;
    }
#endif 

    gInteriorNodesVisited++;

    int nMoves = 0;
    bool isInCheck = false;
    nMoves = countMoves(node, &isInCheck);

    if (nMoves == 0)
    {
        if (isInCheck)
            return -1;
        else
            return 0;
    }

    HexaBitBoardPosition childBoards[MAX_MOVES];
    generateBoards(node, childBoards);

    int bestScore = -INF;
    bool improvedAlpha = false;

    for (int i = 0; i < nMoves; i++)
    {
        int curScore = -alphabeta(&childBoards[i], depth - 1, -beta, -alpha);

        if (curScore >= beta)
        {
#if 1
            storeTT(hash, curScore, SCORE_GE);
#endif
            return beta;
        }

        if (curScore > bestScore)
        {
            bestScore = curScore;
            if (bestChild)
                *bestChild = i;
            if (curScore > alpha)
            {
                improvedAlpha = true;
                alpha = curScore;
            }
        }

    }

#if 1
    // save to hash table
    ttScore = bestScore;

    if (improvedAlpha)
        scoreType = SCORE_EXACT;
    else
        scoreType = SCORE_LE;

    storeTT(hash, ttScore, scoreType);
#endif

    return alpha;
}


bool findMate(HexaBitBoardPosition *pos, int depth)
{
    gInteriorNodesVisited = 0;
    gLeafNodesVisited = 0;
    gNodesMate = 0;

    int bestChild = 0;
    int val = alphabeta(pos, depth, -INF, INF, &bestChild);


    HexaBitBoardPosition childBoards[MAX_MOVES];
    generateBoards(pos, childBoards);

    if (val)
        Utils::displayBoard(&childBoards[bestChild]);

    // not needed
    // memset(transTable, 0, GET_TT_SIZE_FROM_BITS(TT_BITS) * sizeof(uint64));

    return !!val;
}


int main()
{
    BoardPosition testBoard;
    //Utils::readFENString("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", &testBoard); // position 2 of CPW
    //Utils::readFENString("2qrr1n1/3b1kp1/2pBpn1p/1p2PP2/p2P4/1BP5/P3Q1PP/4RRK1 w - - 0 1", &testBoard);   // hard problem
    //Utils::readFENString("8/qQ5p/3pN2K/3pp1R1/4k3/7N/1b1PP3/8 w - - 0 1", &testBoard);  // very easy
    //Utils::readFENString("8/1p3K1p/8/5p2/2Q2P2/k1P4B/3R4/1q6 w - - 0 1", &testBoard); // easy
    //Utils::readFENString("n1N3br/2p1Bpkr/1pP2R1b/pP1p1PpR/Pp4P1/1P6/1K1P4/8 w - - 0 1", &testBoard);   // reasonable - mate found at depth 11 (< 1 second with hash, 10 seconds without)
    //Utils::readFENString("5b1r/Nk1r1pp1/ppNp1q2/7p/2P1Q1n1/6P1/PP3PKP/4RR2 w - - 0 1", &testBoard);    // harder than above. mate in 13 or 20?
    //Utils::readFENString("b5nq/K2Npp2/2pp1Ppr/2pk4/Q1R2pB1/2P1b3/R2p4/n2r4 w - - 0 1", &testBoard);    // mate at depth 11, but took 400 seconds! chest takes 2 seconds!
    Utils::readFENString("5R2/2ppB1p1/8/5pNp/5Nb1/3p3p/3P1P1k/R3K3 w Q - 0 1", &testBoard);              // mate at depth 13, took 380 seconds! chest takes 40 seconds - and checked 100 times less positions!

    Utils::dispBoard(&testBoard);
    HexaBitBoardPosition testBB;
    Utils::board088ToHexBB(&testBB, &testBoard);

    int nMoves = 0;
    bool isInCheck = false;
    MoveGeneratorBitboard::init();
    allocTT();

    for (int i = 0; i < 100; i++)
    {
        bool mate = false;
        START_TIMER
        mate = findMate(&testBB, i);
        STOP_TIMER

        printf("\nNodes (leaf/interior/mate) : %llu / %llu / %llu", gLeafNodesVisited, gInteriorNodesVisited, gNodesMate);
        printf("; Time: %g s\n", gTime);
        if (mate)
        {
            printf("Mate found at depth %d\n", i);
            break;
        }
        else
        {
            printf("NO Mate at depth %d\n", i);
        }
    }

    freeTT();
    MoveGeneratorBitboard::destroy();


    getchar();

    return 0;
}