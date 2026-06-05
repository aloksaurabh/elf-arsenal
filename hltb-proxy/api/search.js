import { HowLongToBeatService } from 'howlongtobeat-ts';

const hltbService = new HowLongToBeatService();

export default async function handler(req, res) {
  // Add CORS headers so the PS5 browser isn't blocked
  res.setHeader('Access-Control-Allow-Credentials', true);
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET,OPTIONS,PATCH,DELETE,POST,PUT');
  res.setHeader(
    'Access-Control-Allow-Headers',
    'X-CSRF-Token, X-Requested-With, Accept, Accept-Version, Content-Length, Content-MD5, Content-Type, Date, X-Api-Version'
  );

  // Handle preflight request
  if (req.method === 'OPTIONS') {
    res.status(200).end();
    return;
  }

  try {
    const name = req.body?.name || req.query?.name;
    
    if (!name) {
      return res.status(400).json({ error: 'Missing game name' });
    }

    const results = await hltbService.search(name);
    
    if (!results.success) {
      return res.status(500).json({ error: 'Search failed or no results' });
    }

    const mappedData = results.data.map(game => ({
      name: game.name,
      game_name: game.name,
      comp_main: game.mainTime || 0,
      comp_plus: game.mainExtraTime || 0,
      comp_100:  game.completionistTime || 0
    }));

    return res.status(200).json({ data: mappedData });
  } catch (error) {
    console.error(error);
    return res.status(500).json({ error: 'Failed to fetch HLTB data' });
  }
}
