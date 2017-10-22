#include <stdio.h>
#include "chess.h"
#include <thread>

#define SKIP_CUDA_CODE
#include "MoveGeneratorBitboard.h"

#define INF 1000


#if USE_TRANSPOSITION_TABLE == 1
#define TT_BITS 27

#define SCORE_EXACT    0
#define SCORE_GE       1
#define SCORE_LE       2

union TTEntryMate
{
    uint64 hash;            // 24 LSBs overlapped with other info
    struct
    {
        CMove bestMove;
        int8  score     : 6;
        uint8 scoreType : 2;
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

bool lookupTT(uint64 hash, int8 *score, uint8 *scoreType, CMove *bestMove)
{
    const uint64 indexBits = GET_TT_INDEX_BITS(TT_BITS);
    const uint64 hashBits  = GET_TT_HASH_BITS(TT_BITS);
    TTEntryMate hashEntry = transTable[hash & indexBits];
    
    if ((hashEntry.hash & hashBits) == (hash & hashBits))
    {
        // hash hit
        *score     = hashEntry.score;
        *scoreType = hashEntry.scoreType;
        *bestMove  = hashEntry.bestMove;
        return true;
    }

    return false;
}

void storeTT(uint64 hash, int8 score, uint8 scoreType, CMove bestMove)
{
    TTEntryMate hashEntry = {};
    hashEntry.hash = hash;
    hashEntry.score = score;
    hashEntry.scoreType = scoreType;
    hashEntry.bestMove = bestMove;
    const uint64 indexBits = GET_TT_INDEX_BITS(TT_BITS);
    transTable[hash & indexBits] = hashEntry;
}
#endif

void randomizeMoves(CMove *moves, int nMoves)
{
    for (int i = 0; i<nMoves; i++)
    {
        int j = std::rand() % nMoves;
        CMove otherMove = moves[j];
        moves[j] = moves[i];
        moves[i] = otherMove;
    }
}

uint64 gInteriorNodesVisited = 0;
uint64 gLeafNodesVisited = 0;
uint64 gNodesMate = 0;

// alpha beta pruning
template <uint8 chance>
int alphabeta(HexaBitBoardPosition *node, uint64 hash, int depth, int alpha, int beta, CMove *bestMove)
{
    if (depth == 0)
    {
        gLeafNodesVisited++;

        if (MoveGeneratorBitboard::isCheckMate<chance>(node))
        {
            gNodesMate++;
            return -1;
        }
        else
        {
            return 0;
        }
    }


    CMove childBestMove = {};
    int bestScore = -INF;
    bool improvedAlpha = false;

#if USE_TRANSPOSITION_TABLE == 1
    // check hash table
    hash ^= (ZOB_KEY(depth) * depth);

    uint8 scoreType;
    int8 ttScore;
    CMove ttMove;
    if (lookupTT(hash, &ttScore, &scoreType, &ttMove))
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
    nMoves = MoveGeneratorBitboard::countMoves<chance>(node, &isInCheck);

    if (nMoves == 0)
    {
        if (isInCheck)
            return -1;
        else
            return 0;
    }


    CMove moves[MAX_MOVES];
    if (depth < 2)
    {
        MoveGeneratorBitboard::generateMoves<chance>(node, moves);
    }
    else
    {
        // move ordering:
        //  1. moves causing check
        //  2. captures
        //  3. other moves

        ExpandedBitBoard ebb;
        ebb = MoveGeneratorBitboard::ExpandBitBoard<chance>(node);

        if (isInCheck)
        {
            int nOutofCheck = MoveGeneratorBitboard::generateMovesOutOfCheck<chance>(&ebb, moves);
            /*
            if (nOutofCheck != nMoves)
                printf("BUG!");
            */
        }
        else
        {
            CMove otherMoves[MAX_MOVES];
            int nChecking = MoveGeneratorBitboard::generateMovesCausingCheck<chance>(&ebb, moves);
            int nCaptures = MoveGeneratorBitboard::generateCaptures<chance>(&ebb, otherMoves);
            int nNormal = MoveGeneratorBitboard::generateNonCaptures<chance>(&ebb, &otherMoves[nCaptures]);
            
            int total = nChecking;
            for (int i = 0; i < nMoves; i++)
            {
                bool found = false;
                for (int j = 0; j < nChecking; j++)
                {
                    if (otherMoves[i].getRAW() == moves[j].getRAW())
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    moves[total++] = otherMoves[i];
            }

            /*
            if ((nCaptures + nNormal != nMoves) || (total != nMoves))
            {
                printf("BUG!");
            }
            */
        }
    }

    for (int i = 0; i < nMoves; i++)
    {
        HexaBitBoardPosition childPos = *node;
        uint64 childHash = hash;
        MoveGeneratorBitboard::makeMove<chance, true>(&childPos, childHash, moves[i]);

        int curScore = -alphabeta<!chance>(&childPos, childHash, depth - 1, -beta, -alpha, &childBestMove);

        if (curScore >= beta)
        {
#if USE_TRANSPOSITION_TABLE == 1
            storeTT(hash, curScore, SCORE_GE, moves[i]);
#endif
            return beta;
        }

        if (curScore > bestScore)
        {
            bestScore = curScore;
            *bestMove = moves[i];

            if (curScore > alpha)
            {
                improvedAlpha = true;
                alpha = curScore;
            }
        }

    }

#if USE_TRANSPOSITION_TABLE == 1
    // save to hash table
    ttScore = bestScore;

    if (improvedAlpha)
        scoreType = SCORE_EXACT;
    else
        scoreType = SCORE_LE;

    storeTT(hash, ttScore, scoreType, *bestMove);
#endif

    return alpha;
}

#define MAX_THREADS 16
std::thread* searchThreads[MAX_THREADS];

void workerthread_start(HexaBitBoardPosition *pos, uint64 hash, int depth)
{
    CMove bestMove;

    if (pos->chance == WHITE)
    {
        alphabeta<WHITE>(pos, hash, depth, -INF, INF, &bestMove);
    }
    else
    {
        alphabeta<BLACK>(pos, hash, depth, -INF, INF, &bestMove);
    }
}

bool findMate(HexaBitBoardPosition *pos, int depth)
{
    gInteriorNodesVisited = 0;
    gLeafNodesVisited = 0;
    gNodesMate = 0;

    uint64 hash = MoveGeneratorBitboard::computeZobristKey(pos);

#if USE_LAZY_SMP == 1
    // Lazy SMP
    // launch multiple threads with the same work
    for (int i = 0; i < MAX_THREADS; i++)
        searchThreads[i] = new std::thread(workerthread_start, pos, hash, depth);

    for (int i = 0; i < MAX_THREADS; i++)
        searchThreads[i]->join();
#endif

    CMove bestMove;
    int val = 0;

    if (pos->chance == WHITE)
    {
        val = alphabeta<WHITE>(pos, hash, depth, -INF, INF, &bestMove);
    }
    else
    {
        val = alphabeta<BLACK>(pos, hash, depth, -INF, INF, &bestMove);
    }

    if (val)
    {
        Utils::displayCompactMove(bestMove);
    }

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
    Utils::readFENString("b5nq/K2Npp2/2pp1Ppr/2pk4/Q1R2pB1/2P1b3/R2p4/n2r4 w - - 0 1", &testBoard);    // mate at depth 11, but took 400 seconds! chest takes 2 seconds! down to 10 seconds with move ordering.
    //Utils::readFENString("5R2/2ppB1p1/8/5pNp/5Nb1/3p3p/3P1P1k/R3K3 w Q - 0 1", &testBoard);              // mate at depth 13, took 380 seconds! chest takes 40 seconds - and checked 100 times less positions! Down to 103 seconds with move ordering

    Utils::dispBoard(&testBoard);
    HexaBitBoardPosition testBB;
    Utils::board088ToHexBB(&testBB, &testBoard);

    int nMoves = 0;
    bool isInCheck = false;
    MoveGeneratorBitboard::init();

#if USE_TRANSPOSITION_TABLE == 1
    allocTT();
#endif

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

#if USE_TRANSPOSITION_TABLE == 1
    freeTT();
#endif
    MoveGeneratorBitboard::destroy();


    getchar();

    return 0;
}