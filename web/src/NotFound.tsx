import { Footer, Nav } from './Nav';

export function NotFound() {
  return (
    <>
      <div className="glow" aria-hidden />
      <Nav />
      <main>
        <section className="hero container">
          <p className="label">404</p>
          <h1>
            Nothing <em>here.</em>
          </h1>
          <p className="lead">This address doesn't lead to a page on vybrant.studio.</p>
          <div className="cta">
            <a href="/" className="btn btn-primary">
              Go to the home page
            </a>
          </div>
        </section>
      </main>
      <Footer />
    </>
  );
}
